/*
 * main.c - Application entry demonstrating a button that safely sends
 * a Generic OnOff message via a work queue, similar to your teacher’s approach,
 * but implemented in a single file with custom code.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/shell.h>    /* For bt_mesh_shell_prov, etc. */
#include <zephyr/bluetooth/mesh/cfg_cli.h>  /* If you want config client ops */

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <stdlib.h>
#include <errno.h>

/* ---------------------------------------------------------------------
 * OnOff opcodes
 * --------------------------------------------------------------------- */
#define OP_ONOFF_GET       BT_MESH_MODEL_OP_2(0x82, 0x01)
#define OP_ONOFF_SET       BT_MESH_MODEL_OP_2(0x82, 0x02)
#define OP_ONOFF_SET_UNACK BT_MESH_MODEL_OP_2(0x82, 0x03)
#define OP_ONOFF_STATUS    BT_MESH_MODEL_OP_2(0x82, 0x04)

/* ---------------------------------------------------------------------
 * Hardware configuration
 * --------------------------------------------------------------------- */
#if DT_NODE_EXISTS(DT_ALIAS(led0))
#define LED0_NODE DT_ALIAS(led0)
#else
#error "No devicetree alias 'led0' found; please define one."
#endif

#define LED0_PIN   DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
static const struct device *const led_dev =
    DEVICE_DT_GET(DT_GPIO_CTLR(LED0_NODE, gpios));

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#define BUTTON_NODE DT_ALIAS(sw0)
#else
#error "No devicetree alias 'sw0' found; please define one."
#endif

#define BUTTON_PIN   DT_GPIO_PIN(BUTTON_NODE, gpios)
#define BUTTON_FLAGS DT_GPIO_FLAGS(BUTTON_NODE, gpios)

/* Make the button device pointer accessible so we can read it in bt_ready(). */
static const struct device *btn_dev;

/* ---------------------------------------------------------------------
 * Defines and values for multiple server and client models
 * --------------------------------------------------------------------- */
static struct bt_mesh_cfg_cli cfg_cli; // optional if you want to do config ops

BT_MESH_SHELL_HEALTH_PUB_DEFINE(health_pub);

/* Simple structure to hold OnOff server state if desired */
static struct {
    bool val;
    uint8_t tid;
} g_onoff_state;

/* Health server callbacks (if you want them) */
static const struct bt_mesh_health_srv_cb health_cb = {
    .attn_on = NULL,
    .attn_off = NULL,
};

static struct bt_mesh_health_srv health_srv = {
    .cb = &health_cb,
};

/* OnOff Client model callback: we only handle the Status opcode. */
static int onoff_client_status_cb(const struct bt_mesh_model *model,
                                  struct bt_mesh_msg_ctx *ctx,
                                  struct net_buf_simple *buf)
{
    uint8_t state_val = net_buf_simple_pull_u8(buf);
    printk("Received Led Status from 0x%04x: %u\n", ctx->addr, state_val);
    return 0;
}

static const struct bt_mesh_model_op onoff_cli_op[] = {
    { OP_ONOFF_STATUS, BT_MESH_LEN_MIN(1), onoff_client_status_cb },
    BT_MESH_MODEL_OP_END,
};

/* OnOff Server callbacks (GET/SET ops). */
static int onoff_srv_get_cb(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    BT_MESH_MODEL_BUF_DEFINE(rsp, OP_ONOFF_STATUS, 1);
    bt_mesh_model_msg_init(&rsp, OP_ONOFF_STATUS);
    net_buf_simple_add_u8(&rsp, g_onoff_state.val);

    /* Respond with the current state. */
    printk("The Led is: val=%u\n", g_onoff_state.val);
    return bt_mesh_model_send(model, ctx, &rsp, NULL, NULL);
}

static int onoff_srv_set_unack_cb(const struct bt_mesh_model *model,
                                  struct bt_mesh_msg_ctx *ctx,
                                  struct net_buf_simple *buf)
{
    uint8_t new_val = net_buf_simple_pull_u8(buf);
    /* 2nd byte is TID typically, skip or store if you want. */

    if (new_val != g_onoff_state.val) {
        g_onoff_state.val = new_val;
        /* Toggle local LED for demonstration. */
        gpio_pin_set(led_dev, LED0_PIN, new_val);
        printk("Turning the led: new_val=%u\n", new_val);
    }
    return 0;
}

static int onoff_srv_set_cb(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    /* Same as set_unack, but we also do a GET response. */
    onoff_srv_set_unack_cb(model, ctx, buf);
    return onoff_srv_get_cb(model, ctx, buf);
}

static const struct bt_mesh_model_op onoff_srv_op[] = {
    { OP_ONOFF_GET,       BT_MESH_LEN_EXACT(0), onoff_srv_get_cb      },
    { OP_ONOFF_SET,       BT_MESH_LEN_MIN(2),   onoff_srv_set_cb      },
    { OP_ONOFF_SET_UNACK, BT_MESH_LEN_MIN(2),   onoff_srv_set_unack_cb},
    BT_MESH_MODEL_OP_END,
};

/* ---------------------------------------------------------------------
 * Defining all the models and their necessities
 * --------------------------------------------------------------------- */
static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),
    BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
    BT_MESH_MODEL_HEALTH_CLI(&bt_mesh_shell_health_cli),

    /* OnOff Server */
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_SRV, onoff_srv_op, NULL, &g_onoff_state),

    /* OnOff Client */
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_CLI, onoff_cli_op, NULL, NULL),
};

static const struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
    .cid        = CONFIG_BT_COMPANY_ID,
    .elem       = elements,
    .elem_count = ARRAY_SIZE(elements),
};

/* ---------------------------------------------------------------------
 * OnOff Client "send" function
 * --------------------------------------------------------------------- */
static int send_onoff_message(bool new_state)
{
    static uint8_t tid;
    struct bt_mesh_msg_ctx ctx = {
        /* Must have the OnOff Client model at root_models[5] bound to an AppKey index */
        .app_idx  = 0, /* We are using 0 for demonstration */
        .addr     = BT_MESH_ADDR_ALL_NODES,
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };

    BT_MESH_MODEL_BUF_DEFINE(msg, OP_ONOFF_SET, 2);
    bt_mesh_model_msg_init(&msg, OP_ONOFF_SET);
    net_buf_simple_add_u8(&msg, new_state);
    net_buf_simple_add_u8(&msg, tid++);

    printk("Sending OnOff=%u\n", new_state);

    int err = bt_mesh_model_send(&root_models[5], &ctx, &msg, NULL, NULL);
    if (err) {
        printk("bt_mesh_model_send() failed, err=%d\n", err);
    }
    return err;
}

/* ---------------------------------------------------------------------
 * Work item for button press
 * --------------------------------------------------------------------- */
static struct k_work button_work;
/* We'll also track a toggle state in code. */
static bool toggle_state;

static void button_work_handler(struct k_work *work)
{
    toggle_state = !toggle_state;
    /* Actually do the mesh send in normal (thread) context. */
    send_onoff_message(toggle_state);
}

/* ---------------------------------------------------------------------
 * Button ISR callback
 * --------------------------------------------------------------------- */
static struct gpio_callback button_cb_data;

static void button_isr_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    /* Defer the mesh call to the work handler. */
    k_work_submit(&button_work);
}

/* ---------------------------------------------------------------------
 * Bluetooth / Mesh initialization callback
 * --------------------------------------------------------------------- */
static void bt_ready(int err)
{
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }

    printk("Bluetooth initialized\n");

    err = bt_mesh_init(&bt_mesh_shell_prov, &comp);
    if (err) {
        printk("Mesh init failed (err %d)\n", err);
        return;
    }

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    printk("Mesh initialized (shell provisioning)\n");

    const struct shell *shell = shell_backend_uart_get_ptr();
    if (shell) {
        k_sleep(K_MSEC(6000));
        shell_execute_cmd(shell, "mesh init");
        k_sleep(K_MSEC(200));

        /* -------------------------------------------------------------
         * Check if button is pressed at startup. If so, reset locally.
         * -------------------------------------------------------------
         */
        /* Note: Depending on board wiring, a "pressed" state might be
         * read as 0 instead of 1. Adjust accordingly if the logic is
         * inverted on your hardware.
         */

        int val = gpio_pin_get(btn_dev, BUTTON_PIN);
        printk("Button pin read: %d\n", val);

        if (val == 1) {
            /* If button is pressed at power on, reset local mesh state. */
            shell_execute_cmd(shell, "mesh reset-local");
        }

        k_sleep(K_MSEC(200));
        shell_execute_cmd(shell, "mesh prov uuid efebeffe");
        k_sleep(K_MSEC(200));
        shell_execute_cmd(shell, "mesh prov pb-gatt on");
    } else {
        printk("Shell backend not initialized\n");
    }
}

/* ---------------------------------------------------------------------
 * Board (LED & Button) init function
 * --------------------------------------------------------------------- */
static int board_init(void)
{
    int err;

    /* LED init */
    if (!device_is_ready(led_dev)) {
        printk("LED device not ready\n");
        return -ENODEV;
    }
    err = gpio_pin_configure(led_dev, LED0_PIN, GPIO_OUTPUT_INACTIVE | LED0_FLAGS);
    if (err) {
        printk("Failed to configure LED0 pin (err %d)\n", err);
        return err;
    }

    /* Button init */
    btn_dev = DEVICE_DT_GET(DT_GPIO_CTLR(BUTTON_NODE, gpios));
    if (!device_is_ready(btn_dev)) {
        printk("Button device not ready\n");
        return -ENODEV;
    }
    err = gpio_pin_configure(btn_dev, BUTTON_PIN, GPIO_INPUT | BUTTON_FLAGS);
    if (err) {
        printk("Failed to configure button pin (err %d)\n", err);
        return err;
    }
    err = gpio_pin_interrupt_configure(btn_dev, BUTTON_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        printk("Failed to configure button interrupt (err %d)\n", err);
        return err;
    }

    gpio_init_callback(&button_cb_data, button_isr_cb, BIT(BUTTON_PIN));
    gpio_add_callback(btn_dev, &button_cb_data);

    return 0;
}

/* ---------------------------------------------------------------------
 * Shell Commands
 * --------------------------------------------------------------------- */

static int cmd_leds(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "Usage: leds <0|1>");
        return -EINVAL;
    }

    char *endptr;
    long onoff_val = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || (onoff_val != 0 && onoff_val != 1)) {
        shell_print(sh, "Invalid on/off value: %s; must be 0 or 1", argv[1]);
        return -EINVAL;
    }

    int err = gpio_pin_set(led_dev, LED0_PIN, onoff_val);
    if (err) {
        shell_print(sh, "Failed to set LED to %ld, error: %d", onoff_val, err);
    } else {
        shell_print(sh, "LED set to: %s", onoff_val ? "on" : "off");
    }

    return err;
}

SHELL_CMD_REGISTER(leds, NULL,
    "Set LED on/off: leds <0|1>",
    cmd_leds);

/* ---------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------- */
int main(void)
{
    int err;

    printk("Initializing...\n");

    /* Initialize board-level hardware: LED & button. */
    err = board_init();
    if (err) {
        printk("board_init failed (err %d)\n", err);
        return 0;
    }

    /* Initialize the work item that handles the button logic. */
    k_work_init(&button_work, button_work_handler);

    /* Initialize Bluetooth. Provide the callback that sets up mesh. */
    err = bt_enable(bt_ready);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }

    /* Optional: start the shell on UART. So you can do "mesh init", etc. */
    const struct shell *shell = shell_backend_uart_get_ptr();
    if (shell) {
        err = shell_start(shell);
        if (err) {
            printk("shell_start() failed (err %d)\n", err);
        }
    } else {
        printk("No UART shell backend found\n");
    }

    printk("Setup complete.\n");

    return 0;
}
