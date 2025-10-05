#include <zephyr/pm/device.h>
#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <errno.h>
#include <string.h>
#include <zephyr/drivers/led.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);
static const struct device *const ldo_en = DEVICE_DT_GET(DT_ALIAS(ldoen));

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


int main(void) {
	const struct device *modem_dev = DEVICE_DT_GET(DT_NODELABEL(modem));

	shutdown_modem_power();
	setup_modem_power();
	if (modem_dev == NULL) {
		LOG_ERR("Modem device not found");

	} else if (!device_is_ready(modem_dev)) {
		LOG_ERR("Modem device not ready! Check init.");
		return -1;
	} else {
		LOG_INF("Modem ready");
	}

	pm_device_action_run(modem_dev, PM_DEVICE_ACTION_RESUME);

	LOG_INF("booting up...");
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	if (!iface) {
		LOG_ERR("No PPP interface found");
		return -1;
	}

	net_if_up(iface);

	struct net_mgmt_event_callback cb;
	net_mgmt_init_event_callback(&cb, NULL, 0);
	net_mgmt_add_event_callback(&cb);

	int timeout = 60;
	while (timeout-- > 0 && !net_if_is_up(iface)) {
		k_sleep(K_SECONDS(1));
	}

	if (!net_if_is_up(iface)) {
		LOG_ERR("PPP connection failed");
		return -1;
	}

	LOG_INF("PPP connected successfully");

	/* Connect to Echo Server */
	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(7);

	inet_pton(AF_INET, "34.192.142.126", &addr.sin_addr);

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Socket creation failed: %s (errno %d)", strerror(errno), errno);
		return -1;
	}

	LOG_INF("Waiting Modem UP");
	k_sleep(K_SECONDS(5));

	for(int i=0; i<5; i++)
	{

		char *test_msg = "Hello CodeZoo!!!";
		char buffer[32];
		ssize_t sent = sendto(sock, test_msg, strlen(test_msg), 0, (struct sockaddr *)&addr, sizeof(addr));
		if (sent < 0) {
			LOG_ERR("UDP send failed: %s (errno %d)", strerror(errno), errno);
			switch (errno) {
				case ENETUNREACH:
					LOG_ERR("Network is unreachable. Check PPP connection and routing.");
					break;
				case EHOSTUNREACH:
					LOG_ERR("Host is unreachable. Check DNS server IP and network.");
					break;
				case EADDRNOTAVAIL:
					LOG_ERR("Address not available. Check local IP configuration.");
					break;
				case ENOBUFS:
					LOG_ERR("No buffer space available. System may be out of memory.");
					break;
				case EACCES:
					LOG_ERR("Permission denied. Check socket permissions.");
					break;
				case EIO:
					LOG_ERR("I/O error. Possible modem or driver issue.");
					break;
				default:
					LOG_ERR("UDP send failed with unspecified error.");
					break;
			}
		} else {
			LOG_INF("UDP test packet sent successfully (%zd bytes)", sent);
		}

		memset(buffer, 0x0, sizeof(buffer));

		ssize_t recv = recvfrom(sock, (char *)buffer, sizeof(buffer), 0, (struct sockaddr *)&addr, sizeof(addr));
		if (recv < 0) {
			close(sock);
			LOG_ERR("UDP recv failed with unspecified error.");
		}

		buffer[recv] = '\0';
		LOG_INF("UDP test receive packet ( %s )",buffer); 
		k_sleep(K_SECONDS(2));
	}
	LOG_INF("UDP Socket close"); 
	zsock_close(sock);

	LOG_INF("UDP Socket Test end"); 
	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}
