/* main.c - Application main entry point */

#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/cfg_cli.h>  /* For bt_mesh_cfg_cli_mod_sub_* APIs */
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/mesh/shell.h>
#include <errno.h>
#include <stdlib.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

/* -------------------------------------------------------------
 * SIG OnOff definitions
 * ------------------------------------------------------------- */
#define OP_ONOFF_GET       BT_MESH_MODEL_OP_2(0x82, 0x01)
#define OP_ONOFF_SET       BT_MESH_MODEL_OP_2(0x82, 0x02)
#define OP_ONOFF_SET_UNACK BT_MESH_MODEL_OP_2(0x82, 0x03)
#define OP_ONOFF_STATUS    BT_MESH_MODEL_OP_2(0x82, 0x04)

/* -------------------------------------------------------------
 * Vendor Model definitions
 * -------------------------------------------------------------
 */
#define MY_COMPANY_ID      0x1234
#define MY_CUSTOM_MODEL_ID 0x0001
#define OP_CUSTOM_MESSAGE  BT_MESH_MODEL_OP_3(0x05, MY_COMPANY_ID)

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
 * Make sure you have a devicetree alias for sw0 or rename it accordingly.
 */
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#define BUTTON0_NODE DT_ALIAS(sw0)
#else
#error "Button0 device alias not found in the device tree."
#endif

#define BUTTON0_PIN   DT_GPIO_PIN(BUTTON0_NODE, gpios)
#define BUTTON0_FLAGS DT_GPIO_FLAGS(BUTTON0_NODE, gpios)

/* Group address to which we'll send the Off command */
#define GROUP_ADDR 0xC000

/* Bluetooth Mesh configuration */
static struct bt_mesh_cfg_cli cfg_cli;  /* If your version needs a separate config client instance */

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

/* 
 * Vendor model receive callback. 
 * This function executes any message as a shell command.
 */
static int receive_custom_message(const struct bt_mesh_model *model,
                                  struct bt_mesh_msg_ctx *ctx,
                                  struct net_buf_simple *buf)
{
    char message[33] = {0}; // Buffer for received message

    size_t len = (buf->len > 32) ? 32 : buf->len;
    memcpy(message, buf->data, len);
    message[len] = '\0';

    printk("Received message from 0x%04x: %s\n", ctx->addr, message);

    // Execute the received message as a shell command
    const struct shell *shell = shell_backend_uart_get_ptr();
    if (!shell) {
        printk("Shell backend not initialized\n");
        return -ENODEV;
    }

    if (shell_execute_cmd(shell, message) != 0) {
        printk("Failed to execute command: %s\n", message);
    }

    return 0;
}

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

/* Vendor model operations: handle OP_CUSTOM_MESSAGE */
static const struct bt_mesh_model_op custom_model_op[] = {
    { OP_CUSTOM_MESSAGE, BT_MESH_LEN_MIN(1), receive_custom_message },
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
 *   3 => OnOff Server
 *   4 => OnOff Client
 *   5 => "Vendor" model using custom_model_op
 */
static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),
    BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
    BT_MESH_MODEL_HEALTH_CLI(&bt_mesh_shell_health_cli),

    /* Our standard OnOff Server */
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_SRV, gen_onoff_srv_op, NULL, &onoff),

    /* Our OnOff Client */
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_CLI, gen_onoff_cli_op, NULL, NULL),

    /* Vendor model (instead of a second OnOff Server) */
    BT_MESH_MODEL_VND(MY_COMPANY_ID, MY_CUSTOM_MODEL_ID,
                      custom_model_op, NULL, NULL),
};

static const struct bt_mesh_comp comp = {
    .cid        = CONFIG_BT_COMPANY_ID,
    .elem       = elements,
    .elem_count = ARRAY_SIZE(elements),
};

/* Provisioning configuration */
static uint8_t dev_uuid[16] = {
    0xbc, 0x9c, 0xdc, 0xaa, 
    0x99, 0x88, 0x77, 0x66,
    0x55, 0x44, 0x33, 0x22,
    0x11, 0x00, 0x00, 0x01
};

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
    printk("Provisioning complete. NetIdx: 0x%04x, Address: 0x%04x\n", net_idx, addr);
}

static void prov_reset(void)
{
    bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static const struct bt_mesh_prov prov = {
    .uuid           = dev_uuid,
    .output_size    = 0,
    .output_actions = 0,
    .complete       = prov_complete,
    .reset          = prov_reset,
};

static const struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};


/* Bluetooth initialization */
static void bt_ready(int err)
{
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }

    printk("Bluetooth initialized\n");

    err = bt_mesh_init(&prov, &comp);
    if (err) {
        printk("Mesh init failed (err %d)\n", err);
        return;
    }

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
    printk("Mesh initialized\n");
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

/* Send OnOff message to all nodes or specific group address */
static int send_onoff_message(bool state)
{
    static uint8_t tid;
    struct bt_mesh_msg_ctx ctx = {
        .app_idx  = 0,
        .addr     = BT_MESH_ADDR_ALL_NODES, /* If you want to send to all nodes */
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };
    BT_MESH_MODEL_BUF_DEFINE(msg, OP_ONOFF_SET, 4);
    bt_mesh_model_msg_init(&msg, OP_ONOFF_SET);
    net_buf_simple_add_u8(&msg, state);
    net_buf_simple_add_u8(&msg, tid++);

    printk("Sending OnOff Set: %s\n", state ? "on" : "off");
    int err = bt_mesh_model_send(&root_models[3], &ctx, &msg, NULL, NULL);
    if (err) {
        printk("Failed to send message (err %d)\n", err);
    }
    return err;
}

/* 
 * If you specifically want to send Off to a group address, 
 * you can add a separate helper:
 */
static int send_onoff_to_group(bool state, uint16_t group_address)
{
    static uint8_t tid_group;
    struct bt_mesh_msg_ctx ctx = {
        .app_idx  = 0,
        .addr     = group_address, /* The group address */
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };
    BT_MESH_MODEL_BUF_DEFINE(msg, OP_ONOFF_SET, 4);
    bt_mesh_model_msg_init(&msg, OP_ONOFF_SET);
    net_buf_simple_add_u8(&msg, state);
    net_buf_simple_add_u8(&msg, tid_group++);

    printk("Sending OnOff Set to group 0x%04x: %s\n",
           group_address, state ? "on" : "off");
    int err = bt_mesh_model_send(&root_models[3], &ctx, &msg, NULL, NULL);
    if (err) {
        printk("Failed to send group message (err %d)\n", err);
    }
    return err;
}

/* ---------------------------------------------------------------------
 * Shell Commands
 * ---------------------------------------------------------------------
 */

/* 1) Control local LED on or off */
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

/* 2) Send a custom message (with spaces) to a unicast address */
static int cmd_sendto(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_print(sh, "Usage: sendto <unicast_address(hex)> <message...>");
        shell_print(sh, "Example: sendto 0002 mod_sub_del 0002 0002 c000 1000");
        return -EINVAL;
    }

    /* Parse unicast address (hex) */
    char *endptr;
    uint16_t unicast_addr = strtol(argv[1], &endptr, 16);
    if (*endptr != '\0' || unicast_addr > 0x7FFF) {
        shell_print(sh, "Invalid unicast address: %s", argv[1]);
        return -EINVAL;
    }

    /* Concatenate everything from argv[2] onward into a single string */
    char message[128];
    message[0] = '\0';

    for (int i = 2; i < argc; i++) {
        if ((strlen(message) + strlen(argv[i]) + 2) >= sizeof(message)) {
            shell_print(sh, "Message too long (max 127 chars)");
            return -EINVAL;
        }
        strcat(message, argv[i]);
        if (i < argc - 1) {
            strcat(message, " ");
        }
    }

    size_t message_len = strlen(message);
    if (message_len == 0) {
        shell_print(sh, "Message cannot be empty");
        return -EINVAL;
    }

    /* Prepare the message context */
    struct bt_mesh_msg_ctx ctx = {
        .app_idx  = 0,       /* Must match an AppKey bound to the vendor model */
        .addr     = unicast_addr,
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };

    /* Build a vendor message using our vendor opcode */
    BT_MESH_MODEL_BUF_DEFINE(msg, OP_CUSTOM_MESSAGE, 128);
    bt_mesh_model_msg_init(&msg, OP_CUSTOM_MESSAGE);
    net_buf_simple_add_mem(&msg, message, message_len);

    shell_print(sh, "Sending message to 0x%04x: \"%s\"", unicast_addr, message);

    /*
     * The vendor model is at root_models[5] in your composition array,
     * so we call &root_models[5] here.
     */
    int err = bt_mesh_model_send(&root_models[5], &ctx, &msg, NULL, NULL);
    if (err) {
        shell_print(sh, "Failed to send message (err %d)", err);
    } else {
        shell_print(sh, "Message sent successfully");
    }

    return err;
}

/* 3) Add or remove model subscription using the config client APIs. */
static uint16_t net_idx = 0x0000; /* Adjust if needed */

/* Usage:
 *   mod_sub_add <node_unicast(hex)> <elem_addr(hex)> <group_addr(hex)> <model_id(hex)>
 */
static int cmd_mod_sub_add(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 5) {
        shell_print(sh, "Usage: mod_sub_add <node_unicast> <elem_addr> <group_addr> <model_id>");
        return -EINVAL;
    }

    char *endptr;
    uint16_t node_unicast = strtol(argv[1], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid node unicast: %s", argv[1]);
        return -EINVAL;
    }

    uint16_t elem_addr = strtol(argv[2], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid element address: %s", argv[2]);
        return -EINVAL;
    }

    uint16_t group_addr = strtol(argv[3], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid group address: %s", argv[3]);
        return -EINVAL;
    }

    uint16_t model_id = strtol(argv[4], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid model ID: %s", argv[4]);
        return -EINVAL;
    }

    uint8_t status;
    int err = bt_mesh_cfg_cli_mod_sub_add(
        net_idx,
        node_unicast,
        elem_addr,
        group_addr,
        model_id,
        &status
    );
    if (err) {
        shell_print(sh, "Failed to send Mod Sub Add (err %d)", err);
        return err;
    }
    if (status) {
        shell_print(sh, "Mod Sub Add failed, status 0x%02x", status);
        return -EIO;
    }

    shell_print(sh, "Subscription added successfully!");
    return 0;
}

/* Usage:
 *   mod_sub_del <node_unicast(hex)> <elem_addr(hex)> <group_addr(hex)> <model_id(hex)>
 */
static int cmd_mod_sub_del(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 5) {
        shell_print(sh, "Usage: mod_sub_del <node_unicast> <elem_addr> <group_addr> <model_id>");
        return -EINVAL;
    }

    char *endptr;
    uint16_t node_unicast = strtol(argv[1], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid node unicast: %s", argv[1]);
        return -EINVAL;
    }

    uint16_t elem_addr = strtol(argv[2], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid element address: %s", argv[2]);
        return -EINVAL;
    }

    uint16_t group_addr = strtol(argv[3], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid group address: %s", argv[3]);
        return -EINVAL;
    }

    uint16_t model_id = strtol(argv[4], &endptr, 16);
    if (*endptr != '\0') {
        shell_print(sh, "Invalid model ID: %s", argv[4]);
        return -EINVAL;
    }

    uint8_t status;
    int err = bt_mesh_cfg_cli_mod_sub_del(
        net_idx,
        node_unicast,
        elem_addr,
        group_addr,
        model_id,
        &status
    );
    if (err) {
        shell_print(sh, "Failed to send Mod Sub Del (err %d)", err);
        return err;
    }
    if (status) {
        shell_print(sh, "Mod Sub Del failed, status 0x%02x", status);
        return -EIO;
    }

    shell_print(sh, "Subscription removed successfully!");
    return 0;
}

/* Register shell commands */
SHELL_CMD_REGISTER(leds, NULL, 
    "Set LED on/off: leds <0|1>", 
    cmd_leds);

SHELL_CMD_REGISTER(sendto, NULL, 
    "Send vendor message (with spaces) to a unicast. Usage:\n"
    "  sendto <unicast(hex)> <msg...>",
    cmd_sendto);

SHELL_CMD_REGISTER(mod_sub_add, NULL, 
    "Add group subscription: mod_sub_add <node_uni> <elem_addr> <group_addr> <model_id>", 
    cmd_mod_sub_add);

SHELL_CMD_REGISTER(mod_sub_del, NULL, 
    "Del group subscription: mod_sub_del <node_uni> <elem_addr> <group_addr> <model_id>", 
    cmd_mod_sub_del);

/* 
 * Button interrupt callback - triggered when the button is pressed.
 * This will send an "off" command to the group address (GROUP_ADDR).
 */
static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button pressed! Turning off all LEDs in group 0x%04x\n", GROUP_ADDR);
    send_onoff_to_group(false, GROUP_ADDR);
}

/* Create a GPIO callback structure */
static struct gpio_callback button_cb_data;

int main(void)
{
    int err;

    printk("Initializing...\n");

    /* Initialize LED */
    if (!device_is_ready(led_dev)) {
        printk("LED device not ready\n");
        return -ENODEV;
    }
    gpio_pin_configure(led_dev, LED0_PIN, GPIO_OUTPUT_ACTIVE | LED0_FLAGS);

    /* Initialize Button */
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

    /* Set up an interrupt on button press (falling edge or rising edge depends on board wiring) */
    err = gpio_pin_interrupt_configure(button_dev, BUTTON0_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        printk("Failed to configure button interrupt (err %d)\n", err);
        return err;
    }

    /* Initialize and add callback */
    gpio_init_callback(&button_cb_data, button_pressed, BIT(BUTTON0_PIN));
    gpio_add_callback(button_dev, &button_cb_data);

    /* Initialize Bluetooth */
    err = bt_enable(bt_ready);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
    }

    return 0;
}
