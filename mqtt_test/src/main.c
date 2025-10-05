/*
 * Copyright (c) 2021-2023 Nordic Semiconductor ASA
 * Modified for Zephyr v4.2 API compatibility
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ppp.h>
#include <zephyr/random/random.h>

#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/led.h>

#define MQTT_MESSAGE_BUFFER_SIZE 256
#define MQTT_CLIENT_ID "zephyr0099"
#define MQTT_PUB_TOPIC "zephyr/mqtt/publisher"
#define MQTT_PUB_MSG_COUNT 5
#define MQTT_PAYLOAD_BUFFER_SIZE 64

LOG_MODULE_REGISTER(mqtt_publisher_modem, LOG_LEVEL_INF);
static const struct device *const ldo_en = DEVICE_DT_GET(DT_ALIAS(ldoen));

/* Buffers for MQTT client. */
static uint8_t rx_buffer[MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_MESSAGE_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

static struct pollfd pfd;
static atomic_t connected;

void setup_modem_power(void)
{
	int ret;

	if (!device_is_ready(ldo_en)) {
		printk("LDO Enable device not ready\n");
		return;
	}

	printk("Turning on Modem LDO...\n");
	ret = led_on(ldo_en, 0);

	if (ret < 0) {
		printk("Failed to turn on LDO\n");
	}
	k_sleep(K_MSEC(100));
}

void shutdown_modem_power(void)
{
	if (!device_is_ready(ldo_en)) {
		return;
	}

	printk("Turning off Modem LDO...\n");

	led_off(ldo_en, 0);
	k_sleep(K_MSEC(2000));
}


static void mqtt_evt_handler(struct mqtt_client *const c,
		const struct mqtt_evt *evt)
{
	switch (evt->type) {
		case MQTT_EVT_CONNACK:
			if (evt->result != 0) {
				LOG_ERR("MQTT connect failed: %d", evt->result);
				break;
			}

			atomic_set(&connected, 1);
			LOG_INF("MQTT client connected");
			break;

		case MQTT_EVT_DISCONNECT:
			LOG_INF("MQTT client disconnected: %d", evt->result);
			atomic_set(&connected, 0);
			pfd.fd = -1;
			break;

		case MQTT_EVT_PUBACK:
			LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);
			break;

		default:
			break;
	}
}

static int client_init(void)
{
	mqtt_client_init(&client);

	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (const uint8_t *)MQTT_CLIENT_ID;
	client.client_id.size = strlen(MQTT_CLIENT_ID);
	client.password = NULL;
	client.user_name = NULL;
	client.protocol_version = MQTT_VERSION_3_1_1;

	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	return 0;
}

static int broker_init(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	err = getaddrinfo("broker.hivemq.com", NULL, &hints, &result);
	if (err) {
		LOG_ERR("getaddrinfo failed: %d", err);
		return -ECHILD;
	}

	struct sockaddr_in *broker4 = (struct sockaddr_in *)result->ai_addr;
	broker4->sin_port = htons(1883);

	memcpy(&broker, broker4, sizeof(struct sockaddr_in));

	freeaddrinfo(result);

	return 0;
}

static void data_publish(struct mqtt_client *c, enum mqtt_qos qos,
		uint8_t *data, size_t len)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = MQTT_PUB_TOPIC;
	param.message.topic.topic.size = strlen(MQTT_PUB_TOPIC);
	param.message.payload.data = data;
	param.message.payload.len = len;
	param.message_id = sys_rand32_get();
	param.dup_flag = 0;
	param.retain_flag = 0;

	mqtt_publish(c, &param);
}

int main(void)
{
	int err;
	uint32_t i = 0;
	char payload[MQTT_PAYLOAD_BUFFER_SIZE];

	const struct device *modem_dev = DEVICE_DT_GET(DT_NODELABEL(modem));

    shutdown_modem_power();
    setup_modem_power();

	if (!device_is_ready(modem_dev)) {
		LOG_ERR("Modem device not ready!");
		return -1;
	}
	LOG_INF("Modem ready");
	pm_device_action_run(modem_dev, PM_DEVICE_ACTION_RESUME);
	LOG_INF("Bringing up PPP interface...");
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	if (!iface) {
		LOG_ERR("No PPP interface found");
		return -1;
	}
	net_if_up(iface);
	int timeout = 60;
	while (timeout-- > 0 && !net_if_is_up(iface)) {
		LOG_INF("Waiting for PPP connection... %d s left", timeout + 1);
		k_sleep(K_SECONDS(1));
	}
	if (!net_if_is_up(iface)) {
		LOG_ERR("PPP connection failed after 60 seconds");
		return -1;
	}
	LOG_INF("PPP connected successfully!");
	LOG_INF("Waiting Modem UP");
	k_sleep(K_SECONDS(5));

	LOG_INF("Initializing MQTT client...");
	err = client_init();
	if (err) {
		LOG_ERR("client_init: %d", err);
		return 0;
	}

	err = broker_init();
	if (err) {
		LOG_ERR("broker_init: %d", err);
		return 0;
	}

	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect: %d", err);
		return 0;
	}

	pfd.fd = client.transport.tcp.sock;
	pfd.events = POLLIN;

	LOG_INF("Waiting for CONNACK...");
	timeout = 10;
	while(timeout-- > 0 && !atomic_get(&connected)) {
		err = poll(&pfd, 1, 1000);
		if (err > 0 && (pfd.revents & POLLIN)) {
			err = mqtt_input(&client);
			if (err) {
				LOG_ERR("mqtt_input: %d", err);
				return 0;
			}
		} else if (err < 0) {
			LOG_ERR("poll failed: %d", errno);
			return 0;
		}
	}

	if (!atomic_get(&connected)) {
		LOG_ERR("Failed to connect to MQTT broker within 10 seconds.");
		return 0;
	}

	while (i < MQTT_PUB_MSG_COUNT && atomic_get(&connected)) {
		err = poll(&pfd, 1, 100); //100ms poll
		if (err < 0) {
			LOG_ERR("poll: %d", errno);
			break;
		}

		if ((pfd.revents & POLLIN) == POLLIN) {
			err = mqtt_input(&client);
			if (err) {
				LOG_ERR("mqtt_input: %d", err);
				break;
			}
		}

		err = mqtt_live(&client);
		if (err != 0 && err != -EAGAIN) {
			LOG_ERR("mqtt_live: %d", err);
			break;
		}

		LOG_INF("Publishing message %u...", i + 1);
		sprintf(payload, "Message %u from Zephyr", ++i);
		data_publish(&client, MQTT_QOS_0_AT_MOST_ONCE, payload, strlen(payload));

		k_sleep(K_SECONDS(2));
	}

	LOG_INF("Disconnecting MQTT client...");

	err = mqtt_disconnect(&client, NULL);
	if (err) {
		LOG_ERR("mqtt_disconnect: %d", err);
	}

	LOG_INF("Bye!");

	return 0;
}	
