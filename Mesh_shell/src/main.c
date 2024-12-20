/* main.c - Application main entry point */

#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/mesh/shell.h>

/* Definitions for On/Off operations */
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

#define LED0_PIN DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
static const struct device *const led_dev = DEVICE_DT_GET(DT_GPIO_CTLR(LED0_NODE, gpios));

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
static int gen_onoff_get(const struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf) {
    BT_MESH_MODEL_BUF_DEFINE(rsp, OP_ONOFF_STATUS, 1);
    bt_mesh_model_msg_init(&rsp, OP_ONOFF_STATUS);
    net_buf_simple_add_u8(&rsp, onoff.val);
    bt_mesh_model_send(model, ctx, &rsp, NULL, NULL);
    return bt_mesh_model_send(model, ctx, &rsp, NULL, NULL);
}

static int gen_onoff_set_unack(const struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf) {
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

static int gen_onoff_set(const struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf) {
    gen_onoff_set_unack(model, ctx, buf);
    gen_onoff_get(model, ctx, buf);
    return 0;
}

static const struct bt_mesh_model_op gen_onoff_srv_op[] = {
    { OP_ONOFF_GET, 0, gen_onoff_get },
    { OP_ONOFF_SET, 2, gen_onoff_set },
    { OP_ONOFF_SET_UNACK, 2, gen_onoff_set_unack },
    BT_MESH_MODEL_OP_END,
};

/* Combined Mesh model definitions */
static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),
    BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_SRV, gen_onoff_srv_op, NULL, &onoff),
    BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_CLI, gen_onoff_cli_op, NULL, NULL),
};

static const struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
    .cid = CONFIG_BT_COMPANY_ID,
    .elem = elements,
    .elem_count = ARRAY_SIZE(elements),
};

/* Provisioning configuration */
static uint8_t dev_uuid[16] = { 0xc1, 0xdd, 0xdc, 0xaa, 0x99, 0x88, 0x77, 0x66,
                                0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x01 };

static void prov_complete(uint16_t net_idx, uint16_t addr) {
    printk("Provisioning complete. NetIdx: 0x%04x, Address: 0x%04x\n", net_idx, addr);
}

static void prov_reset(void) {
    bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static const struct bt_mesh_prov prov = {
    .uuid = dev_uuid,
    .output_size = 0,
    .output_actions = 0,
    .complete = prov_complete,
    .reset = prov_reset,
};

/* Bluetooth initialization */
static void bt_ready(int err) {
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
                            struct net_buf_simple *buf) {
    printk("Message details - Addr: 0x%04x, AppIdx: %d, TTL: %d\n",
           ctx->addr, ctx->app_idx, ctx->send_ttl);
    for (size_t i = 0; i < buf->len; i++) {
        printk("%02x ", buf->data[i]);
    }
    printk("\n");
    return 0;
}

static int send_onoff_message(bool state) {
    static uint8_t tid; // Transaction Identifier
    struct bt_mesh_msg_ctx ctx = {
        .app_idx = 0, // Use the first bound application key
        .addr = BT_MESH_ADDR_ALL_NODES, // Broadcast to all nodes
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };
    BT_MESH_MODEL_BUF_DEFINE(msg, OP_ONOFF_SET, 4); // Change OP code to OP_ONOFF_SET
    bt_mesh_model_msg_init(&msg, OP_ONOFF_SET);
    net_buf_simple_add_u8(&msg, state); // LED state
    net_buf_simple_add_u8(&msg, tid++); // Increment TID for each message

    printk("Sending OnOff Set: %s\n", state ? "on" : "off");
    int err = bt_mesh_model_send(&root_models[4], &ctx, &msg, NULL, NULL);
    if (err) {
        printk("Failed to send message (err %d)\n", err);
    }
    return err;
}

/* Main entry point */
int main(void) {
    int err;

    printk("Initializing...\n");

    if (!device_is_ready(led_dev)) {
        printk("LED device not ready\n");
        return -ENODEV;
    }

    gpio_pin_configure(led_dev, LED0_PIN, GPIO_OUTPUT_ACTIVE | LED0_FLAGS);

    err = bt_enable(bt_ready);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
    }

    return 0;
}