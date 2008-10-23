/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <string.h>

#include <gcutter.h>

#define shutdown inet_shutdown
#include <milter-manager-test-utils.h>
#include <milter/manager/milter-manager-controller.h>
#undef shutdown

void test_negotiate (void);
void test_connect (void);
void test_helo (void);
void test_envelope_from (void);
void test_envelope_receipt (void);

static MilterManagerConfiguration *config;
static MilterManagerController *controller;
static MilterOption *option;

static GCutSpawn *spawn;

static gchar *test_client_path;

static gboolean client_ready;
static gboolean client_negotiated;
static gboolean client_connected;
static gboolean client_greeted;
static gboolean client_envelope_from_received;
static gboolean client_envelope_receipt_received;
static gboolean client_reaped;


static void
add_load_path (const gchar *path)
{
    if (!path)
        return;
    milter_manager_configuration_add_load_path(config, path);
}

void
setup (void)
{
    config = milter_manager_configuration_new(NULL);
    add_load_path(g_getenv("MILTER_MANAGER_CONFIG_DIR"));
    milter_manager_configuration_load(config, "milter-manager.conf");

    controller = milter_manager_controller_new(config);
    option = NULL;

    spawn = NULL;

    test_client_path = g_build_filename(milter_manager_test_get_base_dir(),
                                        "lib",
                                        "milter-test-client.rb",
                                        NULL);

    client_ready = FALSE;
    client_negotiated = FALSE;
    client_connected = FALSE;
    client_greeted = FALSE;
    client_envelope_from_received = FALSE;
    client_envelope_receipt_received = FALSE;
    client_reaped = FALSE;
}

void
teardown (void)
{
    if (config)
        g_object_unref(config);
    if (controller)
        g_object_unref(controller);
    if (option)
        g_object_unref(option);

    if (spawn)
        g_object_unref(spawn);
    if (test_client_path)
        g_free(test_client_path);
}

static void
cb_output_received (GCutSpawn *spawn, const gchar *chunk, gsize size,
                    gpointer user_data)
{
    if (g_str_has_prefix(chunk, "ready")) {
        client_ready = TRUE;
    } else if (g_str_has_prefix(chunk, "receive: negotiate")) {
        client_negotiated = TRUE;
    } else if (g_str_has_prefix(chunk, "receive: connect")) {
        client_connected = TRUE;
    } else if (g_str_has_prefix(chunk, "receive: helo")) {
        client_greeted = TRUE;
    } else if (g_str_has_prefix(chunk, "receive: mail")) {
        client_envelope_from_received = TRUE;
    } else if (g_str_has_prefix(chunk, "receive: rcpt")) {
        client_envelope_receipt_received = TRUE;
    } else {
        GString *string;

        string = g_string_new_len(chunk, size);
        g_print("[OUTPUT] <%s>\n", string->str);
        g_string_free(string, TRUE);
    }
}

static void
cb_error_received (GCutSpawn *spawn, const gchar *chunk, gsize size,
                   gpointer user_data)
{
    GString *string;

    string = g_string_new_len(chunk, size);
    g_print("[ERROR] <%s>\n", string->str);
    g_string_free(string, TRUE);
}

static void
cb_reaped (GCutSpawn *spawn, gint status, gpointer user_data)
{
    client_reaped = TRUE;
}

static GCutSpawn *
make_spawn (const gchar *command, ...)
{
    GCutSpawn *spawn;
    va_list args;

    va_start(args, command);
    spawn = gcut_spawn_new_va_list(command, args);
    va_end(args);

    g_signal_connect(spawn, "output-received",
                     G_CALLBACK(cb_output_received), NULL);
    g_signal_connect(spawn, "error-received",
                     G_CALLBACK(cb_error_received), NULL);
    g_signal_connect(spawn, "reaped", G_CALLBACK(cb_reaped), NULL);

    return spawn;
}

static void
run_spawn (GCutSpawn *spawn)
{
    GError *error = NULL;

    gcut_spawn_run(spawn, &error);
    gcut_assert_error(error);

    while (!client_ready && !client_reaped)
        g_main_context_iteration(NULL, TRUE);
    cut_assert_false(client_reaped);
}

void
test_negotiate (void)
{
    option = milter_option_new(2,
                               MILTER_ACTION_ADD_HEADERS |
                               MILTER_ACTION_CHANGE_BODY,
                               MILTER_STEP_NO_MAIL |
                               MILTER_STEP_NO_REPLY_CONNECT);

    spawn = make_spawn(test_client_path, "--print-status",
                       "--timeout", "0.5", "--port", "10025", NULL);
    run_spawn(spawn);

    milter_manager_controller_negotiate(controller, option);
    cut_assert_true(client_negotiated);
}

void
test_connect (void)
{
    const gchar host_name[] = "mx.local.net";
    gint domain;
    struct sockaddr *address;
    socklen_t address_size;
    GError *error = NULL;

    cut_trace(test_negotiate());

    milter_utils_parse_connection_spec("inet:2929@192.168.1.29",
                                       &domain,
                                       &address,
                                       &address_size,
                                       &error);
    gcut_assert_error(error);

    milter_manager_controller_connect(controller, host_name,
                                      address, address_size);
    g_free(address);
    while (!client_connected && !client_reaped)
        g_main_context_iteration(NULL, TRUE);
    cut_assert_true(client_connected);
}

void
test_helo (void)
{
    const gchar fqdn[] = "delian";

    cut_trace(test_connect());

    milter_manager_controller_helo(controller, fqdn);
    while (!client_greeted && !client_reaped)
        g_main_context_iteration(NULL, TRUE);
    cut_assert_true(client_greeted);
}

void
test_envelope_from (void)
{
    const gchar from[] = "kou+sender@cozmixng.org";

    cut_trace(test_helo());

    milter_manager_controller_envelope_from(controller, from);
    while (!client_envelope_from_received && !client_reaped)
        g_main_context_iteration(NULL, TRUE);
    cut_assert_true(client_envelope_from_received);
}

void
test_envelope_receipt (void)
{
    const gchar receipt[] = "kou+receiver@cozmixng.org";

    cut_trace(test_envelope_from());

    milter_manager_controller_envelope_receipt(controller, receipt);
    while (!client_envelope_receipt_received && !client_reaped)
        g_main_context_iteration(NULL, TRUE);
    cut_assert_true(client_envelope_receipt_received);
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
