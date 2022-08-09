/*
 * Home greenhose
 */

#include <zephyr/zephyr.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/posix/time.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/sntp.h>
#ifdef CONFIG_POSIX_API
#include <arpa/inet.h>
#endif

#include <string.h>

#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_event.h>


#include <zephyr/logging/log.h>

//General parameters
// 1s
#define SLEEP_TIME_MS   1000
// Default stack size for each thread
#define STACKSIZE 1024
// Default scheduling priority 
#define PRIORITY 7

// Pump (GPIO) config
#define PUMP0_NODE DT_ALIAS(pump0)
// Checks if Pump is properly defined on dtb (esp32 needs some tweaking)
#if !DT_NODE_HAS_STATUS(PUMP0_NODE, okay)
#error "Unsupported board: pump0 devicetree alias is not defined"
#endif

// NTP config
#define SNTP_PORT 123
#define SERVER_ADDR "178.215.228.24"

LOG_MODULE_REGISTER(esp32_wifi_sta, LOG_LEVEL_DBG);

static struct net_mgmt_event_callback dhcp_cb;

// General pump struct
struct pump {
	struct gpio_dt_spec spec;
	uint8_t num;
};

// Pump 0
static const struct pump pump0 = {
	.spec = GPIO_DT_SPEC_GET_OR(PUMP0_NODE, gpios, {0}),
	.num = 0,
};

// Functions
// Handler for connection callback
static void handler_cb(struct net_mgmt_event_callback *cb, uint32_t mgmt_event, struct net_if *iface) {

	if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
		return;
	}

	char buf[NET_IPV4_ADDR_LEN];

	LOG_INF("Your address: %s", net_addr_ntop(AF_INET, &iface->config.dhcpv4.requested_ip, buf, sizeof(buf)));
	LOG_INF("Lease time: %u seconds", iface->config.dhcpv4.lease_time);
	LOG_INF("Subnet: %s", net_addr_ntop(AF_INET, &iface->config.ip.ipv4->netmask, buf, sizeof(buf)));
	LOG_INF("Router: %s", net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw, buf, sizeof(buf)));
}

// Test blink to simulate on/off the pump
void blink(const struct pump *pump, uint32_t sleep_ms, uint32_t id)
{
	const struct gpio_dt_spec *spec = &pump->spec;
	uint32_t cnt = 0;
	int32_t ret;

	if (!device_is_ready(spec->port)) {
		printk("Error: %s device is not ready\n", spec->port->name);
		return;
	}

	ret = gpio_pin_configure_dt(spec, GPIO_OUTPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure pin %d (PUMP '%d')\n",
			ret, spec->pin, pump->num);
		return;
	}

	while (1) {
		gpio_pin_set(spec->port, spec->pin, cnt % 2);

		k_msleep(sleep_ms);
		cnt++;
	}
}

// Set system time via NTP
int32_t set_time_sntp () {

	struct sntp_ctx ctx;

	struct sockaddr_in addr;
	struct sntp_time sntp_time;
	int32_t rv;

	struct timespec time_sntp;
	struct timespec actual;
	time_sntp.tv_nsec=0;

	/* ipv4 */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(SNTP_PORT);
	inet_pton(AF_INET, SERVER_ADDR, &addr.sin_addr);

	rv = sntp_init(&ctx, (struct sockaddr *) &addr,
		       sizeof(struct sockaddr_in));
	if (rv < 0) {
		LOG_ERR("Failed to init SNTP IPv4 ctx: %d", rv);
		sntp_close(&ctx);
		return rv;
	}

	LOG_INF("Sending SNTP IPv4 request...");
	rv = sntp_query(&ctx, 4 * MSEC_PER_SEC, &sntp_time);
	if (rv < 0) {
		LOG_ERR("SNTP IPv4 request failed: %d", rv);
		sntp_close(&ctx);
		return rv;		
	} else {
		time_sntp.tv_sec=sntp_time.seconds;
		clock_settime(CLOCK_MONOTONIC, &time_sntp);
		LOG_INF("status: %d", rv);
		LOG_INF("time since Epoch: high word: %u, low word: %u",
			(uint32_t)(sntp_time.seconds >> 32), (uint32_t)sntp_time.seconds);
	}
 
	sntp_close(&ctx);

	clock_gettime(CLOCK_MONOTONIC, &actual);
	LOG_INF("clock_gettime returned %lld\n", actual.tv_sec);

}

// Threads
// Pump thread
void pump0_th(void)
{
	blink(&pump0, 10000, 0);
}

// Wifi thread
void wifi_th(void){
	struct net_if *iface;

	net_mgmt_init_event_callback(&dhcp_cb, handler_cb,
				     NET_EVENT_IPV4_DHCP_BOUND);

	net_mgmt_add_event_callback(&dhcp_cb);

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("wifi interface not available");
		return;
	}

	net_dhcpv4_start(iface);

	if (!IS_ENABLED(CONFIG_ESP32_WIFI_STA_AUTO)) {
		wifi_config_t wifi_config = {
			.sta = {
				.ssid = CONFIG_ESP32_WIFI_SSID,
				.password = CONFIG_ESP32_WIFI_PASSWORD,
			},
		};

		esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);

		ret |= esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
		ret |= esp_wifi_connect();
		if (ret != ESP_OK) {
			LOG_ERR("connection failed");
		}
	}

	k_busy_wait(SLEEP_TIME_MS*5);

	while (1){
		printk("sntp: %d", set_time_sntp());
		k_msleep(SLEEP_TIME_MS*5);
	}
}

K_THREAD_STACK_DEFINE(pump0_stack_area, STACKSIZE);
static struct k_thread pump0_data;

K_THREAD_STACK_DEFINE(ntp_stack_area, STACKSIZE);
static struct k_thread ntp_data;

// Main
void main(void) {

	// Pump thread start-up
	k_tid_t pump0_tid = k_thread_create(&pump0_data, pump0_stack_area, 
							K_THREAD_STACK_SIZEOF(pump0_stack_area),
							pump0_th, NULL, NULL, NULL,
							PRIORITY, 0, K_FOREVER);

	k_thread_name_set(&pump0_data, "pump_0_th");

	// Wifi thread start-up
	k_tid_t ntp_tid = k_thread_create(&ntp_data, ntp_stack_area, 
							K_THREAD_STACK_SIZEOF(ntp_stack_area),
							wifi_th, NULL, NULL, NULL,
							PRIORITY, 0, K_FOREVER);

	k_thread_name_set(&ntp_data, "wifi_ntp_th");

	k_thread_start(&pump0_data);
	k_thread_start(&ntp_data);

	while (1){

		k_msleep(100);
		// PoC thread wakes up
		// k_wakeup(pump0_tid);

	}

}
