/* main.c - Application main entry point */

#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/cfg_cli.h>
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/mesh/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <errno.h>
#include <stdlib.h>

/* -------------------------------------------------------------
 * SIG OnOff definitions
 * ------------------------------------------------------------- */
#define OP_ONOFF_GET       BT_MESH_MODEL_OP_2(0x82, 0x01)
#define OP_ONOFF_SET       BT_MESH_MODEL_OP_2(0x82, 0x02)
#define OP_ONOFF_SET_UNACK BT_MESH_MODEL_OP_2(0x82, 0x03)
#define OP_ONOFF_STATUS    BT_MESH_MODEL_OP_2(0x82, 0x04)

/* LED configuration based on the device tree */
#if DT_NODE_EXISTS(DT_ALIAS(led0))
#define LED0_NODE DT_ALIAS(led0)
#else
#error "LED0 device alias not found in the device tree."
#endif

#define LED0_PIN   DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
static const struct device *const led_dev = DEVICE_DT_GET(DT_GPIO_CTLR(LED0_NODE, gpios));

/* -----------------------------------------------------------------------
 * Button configuration
 * -----------------------------------------------------------------------
 */
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#define BUTTON0_NODE DT_ALIAS(sw0)
#else
#error "Button0 device alias not found in the device tree."
#endif

#define BUTTON0_PIN   DT_GPIO_PIN(BUTTON0_NODE, gpios)
#define BUTTON0_FLAGS DT_GPIO_FLAGS(BUTTON0_NODE, gpios)

/* Group address to which we'll send the OnOff commands */
#define GROUP_ADDR 0xC000

/* Bluetooth Mesh configuration */
static struct bt_mesh_cfg_cli cfg_cli;

BT_MESH_SHELL_HEALTH_PUB_DEFINE(health_pub);

static struct {
    bool val;
    uint8_t tid;
    uint16_t src;
    uint32_t transition_time;
    struct k_work_delayable work;
} onoff;

/* Forward declarations */
static int send_onoff_message(bool state);
static int gen_onoff_status(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf);

/* Health server callbacks */
static const struct bt_mesh_health_srv_cb health_cb = {
    .attn_on = NULL,
    .attn_off = NULL,
};

static struct bt_mesh_health_srv health_srv = {
    .cb = &health_cb,
};

/* OnOff Client Model operations */
static const struct bt_mesh_model_op gen_onoff_cli_op[] = {
    { OP_ONOFF_STATUS, BT_MESH_LEN_MIN(1), gen_onoff_status },
    BT_MESH_MODEL_OP_END,
};

/* OnOff Server Model operations */
static int gen_onoff_get(const struct bt_mesh_model *model,
                         struct bt_mesh_msg_ctx *ctx,
                         struct net_buf_simple *buf)
{
    BT_MESH_MODEL_BUF_DEFINE(rsp, OP_ONOFF_STATUS, 1);
    bt_mesh_model_msg_init(&rsp, OP_ONOFF_STATUS);
    net_buf_simple_add_u8(&rsp, onoff.val);
    bt_mesh_model_send(model, ctx, &rsp, NULL, NULL);
    return 0;
}

static int gen_onoff_set_unack(const struct bt_mesh_model *model,
                               struct bt_mesh_msg_ctx *ctx,
                               struct net_buf_simple *buf)
{
    uint8_t val = net_buf_simple_pull_u8(buf);
    if (val != onoff.val) {
        onoff.val = val;
        printk("LED set to: %s\n", val ? "on" : "off");
        gpio_pin_set(led_dev, LED0_PIN, val);

        // Send the updated state to the mesh network
        send_onoff_message(val);
    }
    return 0;
}

static int gen_onoff_set(const struct bt_mesh_model *model,
                         struct bt_mesh_msg_ctx *ctx,
                         struct net_buf_simple *buf)
{
    gen_onoff_set_unack(model, ctx, buf);
    gen_onoff_get(model, ctx, buf);
    return 0;
}

static const struct bt_mesh_model_op gen_onoff_srv_op[] = {
    { OP_ONOFF_GET,       0, gen_onoff_get       },
    { OP_ONOFF_SET,       2, gen_onoff_set       },
    { OP_ONOFF_SET_UNACK, 2, gen_onoff_set_unack },
    BT_MESH_MODEL_OP_END,
};

/*
 * Mesh model definitions
 *
 * Indices:
 *   0 => Config Server
 *   1 => Config Client
 *   2 => Health Server
 *   3 => Health Client
 *   4 => OnOff Server
 *   5 => OnOff Client
 */
static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),
    BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
    BT_MESH_MODEL_HEALTH_CLI(&bt_mesh_shell_health_cli),

    /* Generic OnOff Server */
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_SRV, gen_onoff_srv_op, NULL, &onoff),

    /* Generic OnOff Client */
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_CLI, gen_onoff_cli_op, NULL, NULL),
};

static const struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
    .cid        = CONFIG_BT_COMPANY_ID,
    .elem       = elements,
    .elem_count = ARRAY_SIZE(elements),
};

/* Bluetooth initialization callback */
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
}

/* OnOff Client Callback */
static int gen_onoff_status(const struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    printk("Message details - Addr: 0x%04x, AppIdx: %d, TTL: %d\n",
           ctx->addr, ctx->app_idx, ctx->send_ttl);
    for (size_t i = 0; i < buf->len; i++) {
        printk("%02x ", buf->data[i]);
    }
    printk("\n");
    return 0;
}

/* Send OnOff message to all nodes or specific address */
static int send_onoff_message(bool state)
{
    static uint8_t tid;
    struct bt_mesh_msg_ctx ctx = {
        .app_idx  = 0,  /* Ensure OnOff Client is bound to AppKey 0 */
        .addr     = BT_MESH_ADDR_ALL_NODES,
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };
    BT_MESH_MODEL_BUF_DEFINE(msg, OP_ONOFF_SET, 4);
    bt_mesh_model_msg_init(&msg, OP_ONOFF_SET);
    net_buf_simple_add_u8(&msg, state);
    net_buf_simple_add_u8(&msg, tid++);

    printk("Sending OnOff Set: %s\n", state ? "on" : "off");

    /* Use the OnOff Client at root_models[5] */
    int err = bt_mesh_model_send(&root_models[5], &ctx, &msg, NULL, NULL);
    if (err) {
        printk("Failed to send message (err %d)\n", err);
    }
    return err;
}

/* Send OnOff message to a group address */
static int send_onoff_to_group(bool state, uint16_t group_address)
{
    static uint8_t tid_group;
    struct bt_mesh_msg_ctx ctx = {
        .app_idx  = 0,  /* Ensure OnOff Client is bound to AppKey 0 */
        .addr     = group_address,
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };
    BT_MESH_MODEL_BUF_DEFINE(msg, OP_ONOFF_SET, 4);
    bt_mesh_model_msg_init(&msg, OP_ONOFF_SET);
    net_buf_simple_add_u8(&msg, state);
    net_buf_simple_add_u8(&msg, tid_group++);

    printk("Sending OnOff Set to group 0x%04x: %s\n",
           group_address, state ? "on" : "off");
    int err = bt_mesh_model_send(&root_models[5], &ctx, &msg, NULL, NULL);
    if (err) {
        printk("Failed to send group message (err %d)\n", err);
    }
    return err;
}

/* ---------------------------------------------------------------------
 * Shell Commands
 * ---------------------------------------------------------------------
 */

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

static int cmd_mod_sub_add(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 5) {
        shell_print(sh, "Usage: mod_sub_add <node_unicast> <elem_addr> <group_addr> <model_id>");
        return -EINVAL;
    }
    /* Parsing and cfg_cli operations as before... */
    /* ... */
    return 0;
}

static int cmd_mod_sub_del(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 5) {
        shell_print(sh, "Usage: mod_sub_del <node_unicast> <elem_addr> <group_addr> <model_id>");
        return -EINVAL;
    }
    /* Parsing and cfg_cli operations as before... */
    /* ... */
    return 0;
}

SHELL_CMD_REGISTER(leds, NULL,
    "Set LED on/off: leds <0|1>",
    cmd_leds);

SHELL_CMD_REGISTER(mod_sub_add, NULL,
    "Add group subscription: mod_sub_add <node_uni> <elem_addr> <group_addr> <model_id>",
    cmd_mod_sub_add);

SHELL_CMD_REGISTER(mod_sub_del, NULL,
    "Del group subscription: mod_sub_del <node_uni> <elem_addr> <group_addr> <model_id>",
    cmd_mod_sub_del);

/* ---- Button Handling ---- */
static bool group_led_state = false;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    group_led_state = !group_led_state;
    printk("Button pressed! Turning %s all LEDs in group 0x%04x\n",
           group_led_state ? "on" : "off", GROUP_ADDR);
    send_onoff_to_group(group_led_state, GROUP_ADDR);
}

static struct gpio_callback button_cb_data;

int main(void)
{
    int err;

    printk("Initializing...\n");

    if (!device_is_ready(led_dev)) {
        printk("LED device not ready\n");
        return -ENODEV;
    }
    gpio_pin_configure(led_dev, LED0_PIN, GPIO_OUTPUT_ACTIVE | LED0_FLAGS);

    const struct device *button_dev = DEVICE_DT_GET(DT_GPIO_CTLR(BUTTON0_NODE, gpios));
    if (!device_is_ready(button_dev)) {
        printk("Button device not ready\n");
        return -ENODEV;
    }

    err = gpio_pin_configure(button_dev, BUTTON0_PIN,
                             GPIO_INPUT | BUTTON0_FLAGS);
    if (err) {
        printk("Failed to configure button pin (err %d)\n", err);
        return err;
    }

    err = gpio_pin_interrupt_configure(button_dev, BUTTON0_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        printk("Failed to configure button interrupt (err %d)\n", err);
        return err;
    }

    gpio_init_callback(&button_cb_data, button_pressed, BIT(BUTTON0_PIN));
    gpio_add_callback(button_dev, &button_cb_data);

    err = bt_enable(bt_ready);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
    }

    return 0;
}
