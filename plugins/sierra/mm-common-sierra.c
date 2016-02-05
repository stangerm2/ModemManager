/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Lanedo GmbH
 */

#include <stdlib.h>
#include <string.h>

#include "mm-common-sierra.h"
#include "mm-base-modem-at.h"
#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-sim-sierra.h"

static MMIfaceModem *iface_modem_parent;

/*****************************************************************************/
/* Custom init and port type hints */

#define TAG_SIERRA_APP_PORT       "sierra-app-port"
#define TAG_SIERRA_APP1_PPP_OK    "sierra-app1-ppp-ok"

gboolean
mm_common_sierra_grab_port (MMPlugin *self,
                            MMBaseModem *modem,
                            MMPortProbe *probe,
                            GError **error)
{
    MMPortSerialAtFlag pflags = MM_PORT_SERIAL_AT_FLAG_NONE;
    MMPortType ptype;

    ptype = mm_port_probe_get_port_type (probe);

    /* Is it a GSM secondary port? */
    if (g_object_get_data (G_OBJECT (probe), TAG_SIERRA_APP_PORT)) {
        if (g_object_get_data (G_OBJECT (probe), TAG_SIERRA_APP1_PPP_OK))
            pflags = MM_PORT_SERIAL_AT_FLAG_PPP;
        else
            pflags = MM_PORT_SERIAL_AT_FLAG_SECONDARY;
    } else if (ptype == MM_PORT_TYPE_AT)
        pflags = MM_PORT_SERIAL_AT_FLAG_PRIMARY;

    return mm_base_modem_grab_port (modem,
                                    mm_port_probe_get_port_subsys (probe),
                                    mm_port_probe_get_port_name (probe),
                                    mm_port_probe_get_parent_path (probe),
                                    ptype,
                                    pflags,
                                    error);
}

gboolean
mm_common_sierra_port_probe_list_is_icera (GList *probes)
{
    GList *l;

    for (l = probes; l; l = g_list_next (l)) {
        /* Only assume the Icera probing check is valid IF the port is not
         * secondary. This will skip the stupid ports which reply OK to every
         * AT command, even the one we use to check for Icera support */
        if (mm_port_probe_is_icera (MM_PORT_PROBE (l->data)) &&
            !g_object_get_data (G_OBJECT (l->data), TAG_SIERRA_APP_PORT))
            return TRUE;
    }

    return FALSE;
}

typedef struct {
    MMPortProbe *probe;
    MMPortSerialAt *port;
    GCancellable *cancellable;
    GSimpleAsyncResult *result;
    guint retries;
} SierraCustomInitContext;

static void
sierra_custom_init_context_complete_and_free (SierraCustomInitContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);

    if (ctx->cancellable)
        g_object_unref (ctx->cancellable);
    g_object_unref (ctx->port);
    g_object_unref (ctx->probe);
    g_object_unref (ctx->result);
    g_slice_free (SierraCustomInitContext, ctx);
}

gboolean
mm_common_sierra_custom_init_finish (MMPortProbe *probe,
                                     GAsyncResult *result,
                                     GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

static void sierra_custom_init_step (SierraCustomInitContext *ctx);

static void
gcap_ready (MMPortSerialAt *port,
            GAsyncResult *res,
            SierraCustomInitContext *ctx)
{
    const gchar *response;
    GError *error = NULL;

    response = mm_port_serial_at_command_finish (port, res, &error);
    if (error) {
        /* If consumed all tries and the last error was a timeout, assume the
         * port is not AT */
        if (ctx->retries == 0 &&
            g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT)) {
            mm_port_probe_set_result_at (ctx->probe, FALSE);
        }
        /* If reported a hard parse error, this port is definitely not an AT
         * port, skip trying. */
        else if (g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_PARSE_FAILED)) {
            mm_port_probe_set_result_at (ctx->probe, FALSE);
            ctx->retries = 0;
        }
        /* Some Icera-based devices (eg, USB305) have an AT-style port that
         * replies to everything with ERROR, so tag as unsupported; sometimes
         * the real AT ports do this too, so let a retry tag the port as
         * supported if it responds correctly later. */
        else if (g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_UNKNOWN)) {
            mm_port_probe_set_result_at (ctx->probe, FALSE);
        }

        /* Just retry... */
        sierra_custom_init_step (ctx);
        goto out;
    }

    /* A valid reply to ATI tells us this is an AT port already */
    mm_port_probe_set_result_at (ctx->probe, TRUE);

    /* Sierra APPx ports have limited AT command parsers that just reply with
     * "OK" to most commands.  These can sometimes be used for PPP while the
     * main port is used for status and control, but older modems tend to crash
     * or fail PPP.  So we whitelist modems that are known to allow PPP on the
     * secondary APP ports.
     */
    if (strstr (response, "APP1")) {
        g_object_set_data (G_OBJECT (ctx->probe), TAG_SIERRA_APP_PORT, GUINT_TO_POINTER (TRUE));

        /* PPP-on-APP1-port whitelist */
        if (strstr (response, "C885") ||
            strstr (response, "USB 306") ||
            strstr (response, "MC8790"))
            g_object_set_data (G_OBJECT (ctx->probe), TAG_SIERRA_APP1_PPP_OK, GUINT_TO_POINTER (TRUE));

        /* For debugging: let users figure out if their device supports PPP
         * on the APP1 port or not.
         */
        if (getenv ("MM_SIERRA_APP1_PPP_OK")) {
            mm_dbg ("Sierra: APP1 PPP OK '%s'", response);
            g_object_set_data (G_OBJECT (ctx->probe), TAG_SIERRA_APP1_PPP_OK, GUINT_TO_POINTER (TRUE));
        }
    } else if (strstr (response, "APP2") ||
               strstr (response, "APP3") ||
               strstr (response, "APP4")) {
        /* Additional APP ports don't support most AT commands, so they cannot
         * be used as the primary port.
         */
        g_object_set_data (G_OBJECT (ctx->probe), TAG_SIERRA_APP_PORT, GUINT_TO_POINTER (TRUE));
    }

    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    sierra_custom_init_context_complete_and_free (ctx);

out:
    if (error)
        g_error_free (error);
}

static void
sierra_custom_init_step (SierraCustomInitContext *ctx)
{
    /* If cancelled, end */
    if (g_cancellable_is_cancelled (ctx->cancellable)) {
        mm_dbg ("(Sierra) no need to keep on running custom init in '%s'",
                mm_port_get_device (MM_PORT (ctx->port)));
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        sierra_custom_init_context_complete_and_free (ctx);
        return;
    }

    if (ctx->retries == 0) {
        mm_dbg ("(Sierra) Couldn't get port type hints from '%s'",
                mm_port_get_device (MM_PORT (ctx->port)));
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        sierra_custom_init_context_complete_and_free (ctx);
        return;
    }

    ctx->retries--;
    mm_port_serial_at_command (
        ctx->port,
        "ATI",
        3,
        FALSE, /* raw */
        FALSE, /* allow_cached */
        ctx->cancellable,
        (GAsyncReadyCallback)gcap_ready,
        ctx);
}

void
mm_common_sierra_custom_init (MMPortProbe *probe,
                              MMPortSerialAt *port,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    SierraCustomInitContext *ctx;

    ctx = g_slice_new (SierraCustomInitContext);
    ctx->result = g_simple_async_result_new (G_OBJECT (probe),
                                             callback,
                                             user_data,
                                             mm_common_sierra_custom_init);
    ctx->probe = g_object_ref (probe);
    ctx->port = g_object_ref (port);
    ctx->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
    ctx->retries = 3;

    sierra_custom_init_step (ctx);
}

/*****************************************************************************/
/* Modem power up (Modem interface) */

gboolean
mm_common_sierra_modem_power_up_finish (MMIfaceModem *self,
                                        GAsyncResult *res,
                                        GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static gboolean
sierra_power_up_wait_cb (GSimpleAsyncResult *result)
{
    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete (result);
    g_object_unref (result);
    return G_SOURCE_REMOVE;
}

static void
cfun_enable_ready (MMBaseModem *self,
                   GAsyncResult *res,
                   GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    guint i;
    const gchar **drivers;
    gboolean is_new_sierra = FALSE;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error)) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    /* Many Sierra devices return OK immediately in response to CFUN=1 but
     * need some time to finish powering up, otherwise subsequent commands
     * may return failure or even crash the modem.  Give more time for older
     * devices like the AC860 and C885, which aren't driven by the 'sierra_net'
     * driver.  Assume any DirectIP (ie, sierra_net) device is new enough
     * to allow a lower timeout.
     */
    drivers = mm_base_modem_get_drivers (MM_BASE_MODEM (self));
    for (i = 0; drivers[i]; i++) {
        if (g_str_equal (drivers[i], "sierra_net")) {
            is_new_sierra = TRUE;
            break;
        }
    }

    /* The modem object will be valid in the callback as 'result' keeps a
     * reference to it. */
    g_timeout_add_seconds (is_new_sierra ? 5 : 10, (GSourceFunc)sierra_power_up_wait_cb, simple);
}

static void
pcstate_enable_ready (MMBaseModem *self,
                      GAsyncResult *res,
                      GSimpleAsyncResult *simple)
{
    /* Ignore errors for now; we're not sure if all Sierra CDMA devices support
     * at!pcstate.
     */
    mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, NULL);

    g_simple_async_result_set_op_res_gboolean (simple, TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

void
mm_common_sierra_modem_power_up (MMIfaceModem *self,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        mm_common_sierra_modem_power_up);

    /* For CDMA modems, run !pcstate */
    if (mm_iface_modem_is_cdma_only (self)) {
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "!pcstate=1",
                                  5,
                                  FALSE,
                                  (GAsyncReadyCallback)pcstate_enable_ready,
                                  result);
        return;
    }

    mm_warn ("Not in full functionality status, power-up command is needed. "
             "Note that it may reboot the modem.");

    /* Try to go to full functionality mode without rebooting the system.
     * Works well if we previously switched off the power with CFUN=4
     */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+CFUN=1,0", /* ",0" requests no reset */
                              10,
                              FALSE,
                              (GAsyncReadyCallback)cfun_enable_ready,
                              result);
}

/*****************************************************************************/
/* Power state loading (Modem interface) */

MMModemPowerState
mm_common_sierra_load_power_state_finish (MMIfaceModem *self,
                                          GAsyncResult *res,
                                          GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_POWER_STATE_UNKNOWN;

    return (MMModemPowerState)GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
}

static void
parent_load_power_state_ready (MMIfaceModem *self,
                               GAsyncResult *res,
                               GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    MMModemPowerState state;

    state = iface_modem_parent->load_power_state_finish (self, res, &error);
    if (error)
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gpointer (simple, GUINT_TO_POINTER (state), NULL);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
pcstate_query_ready (MMBaseModem *self,
                     GAsyncResult *res,
                     GSimpleAsyncResult *simple)
{
    const gchar *result;
    guint state;
    GError *error = NULL;

    result = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!result) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    /* Parse power state reply */
    result = mm_strip_tag (result, "!PCSTATE:");
    if (!mm_get_uint_from_str (result, &state)) {
        g_simple_async_result_set_error (simple,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Failed to parse !PCSTATE response '%s'",
                                         result);
    } else {
        switch (state) {
        case 0:
            g_simple_async_result_set_op_res_gpointer (simple, GUINT_TO_POINTER (MM_MODEM_POWER_STATE_LOW), NULL);
            break;
        case 1:
            g_simple_async_result_set_op_res_gpointer (simple, GUINT_TO_POINTER (MM_MODEM_POWER_STATE_ON), NULL);
            break;
        default:
            g_simple_async_result_set_error (simple,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Unhandled power state: '%u'",
                                             state);
            break;
        }
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

void
mm_common_sierra_load_power_state (MMIfaceModem *self,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        mm_common_sierra_load_power_state);

    /* Check power state with AT!PCSTATE? */
    if (mm_iface_modem_is_cdma_only (self)) {
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "!pcstate?",
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)pcstate_query_ready,
                                  result);
        return;
    }

    /* Otherwise run parent's */
    iface_modem_parent->load_power_state (self,
                                          (GAsyncReadyCallback)parent_load_power_state_ready,
                                          result);
}

/*****************************************************************************/
/* Create SIM (Modem interface) */

MMBaseSim *
mm_common_sierra_create_sim_finish (MMIfaceModem *self,
                                    GAsyncResult *res,
                                    GError **error)
{
    return mm_sim_sierra_new_finish (res, error);
}

void
mm_common_sierra_create_sim (MMIfaceModem *self,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    /* New Sierra SIM */
    mm_sim_sierra_new (MM_BASE_MODEM (self),
                       NULL, /* cancellable */
                       callback,
                       user_data);
}

/*****************************************************************************/
/* Setup ports */

void
mm_common_sierra_setup_ports (MMBaseModem *self)
{
    MMPortSerialAt *ports[2];
    guint i;
    GRegex *pacsp_regex;

    pacsp_regex = g_regex_new ("\\r\\n\\+PACSP.*\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    ports[0] = mm_base_modem_peek_port_primary (self);
    ports[1] = mm_base_modem_peek_port_secondary (self);

    for (i = 0; i < 2; i++) {
        if (!ports[i])
            continue;

        if (i == 1) {
            /* Built-in echo removal conflicts with the APP1 port's limited AT
             * parser, which doesn't always prefix responses with <CR><LF>.
             */
            g_object_set (ports[i],
                          MM_PORT_SERIAL_AT_REMOVE_ECHO, FALSE,
                          NULL);
        }

        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i],
            pacsp_regex,
            NULL, NULL, NULL);
    }

    g_regex_unref (pacsp_regex);
}

/*****************************************************************************/

void
mm_common_sierra_peek_parent_interfaces (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);
}
