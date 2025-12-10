/*********************
 *      INCLUDES
 *********************/
#include "nongye.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>

/* ---------- LED 控制宏定义 ---------- */
#define TEST_MAGIC 'x'
#define LED1 _IO(TEST_MAGIC, 0)
#define LED2 _IO(TEST_MAGIC, 1)
#define LED3 _IO(TEST_MAGIC, 2)
#define LED4 _IO(TEST_MAGIC, 3)
#define LED_ON  0  // 灯亮
#define LED_OFF 1  // 灯灭

/* ---------- 蜂鸣器控制宏定义 ---------- */
#define BEEP_ON  0  // 蜂鸣器开启
#define BEEP_OFF 1  // 蜂鸣器关闭

/* ---------- TCP 服务器配置 ---------- */
#define SERVER_IP   "192.168.5.42"  // 服务器 IP
#define SERVER_PORT 60005           // 服务器端口号

/* ---------- 静态变量 ---------- */
static lv_obj_t *lab_temp = NULL;
static lv_obj_t *lab_humi = NULL;
static lv_obj_t *lab_co2  = NULL;
static lv_obj_t *lab_lux  = NULL;
static lv_obj_t *sw_main  = NULL;
static lv_obj_t *sw_aux   = NULL;
static lv_obj_t *bar_co2  = NULL;
static lv_obj_t *lab_time_header = NULL;
static lv_obj_t *lab_weather_header = NULL;
static lv_obj_t *chart_gas = NULL;
static lv_obj_t *chart_env = NULL;
static int led_fd = -1;
static int beep_fd = -1;
static int socket_fd = -1; // TCP 套接字文件描述符
static lv_obj_t *auto_img = NULL;
static lv_timer_t *img_timer = NULL;
static int auto_idx = 0;
static pthread_t recv_tid;

static struct {
    float temp;
    float humi;
    int   co2;
    int   lux;
    bool  led_main;
    bool  led_aux;
    char  time[8];
    lv_coord_t co2_samples[5];
    lv_coord_t lux_samples[5];
    lv_coord_t temp_samples[5];
    lv_coord_t humi_samples[5];
    int   sample_count;
} g_data = { .temp = 25.0f, .humi = 60.0f, .co2 = 600, .lux = 7000,
             .led_main = false, .led_aux = false, .time = "00:00",
             .co2_samples = {0}, .lux_samples = {0},
             .temp_samples = {0}, .humi_samples = {0}, .sample_count = 0 };

static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t refresh_tid;
static volatile bool quit_refresh = false;

// 自定义指令队列
#define QUEUE_SIZE 10
static char command_queue[QUEUE_SIZE][32]; // 存储最多 10 条指令，每条最多 32 字节
static int queue_head = 0, queue_tail = 0;
static volatile bool queue_full = false;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 图片资源数组（假设图片已转换为 LVGL 格式或使用文件路径） */
static const char *auto_imgs[] = {
    "S:/root/tmp/1.jpg",
    "S:/root/tmp/2.jpg",
    "S:/root/tmp/3.jpg",
};

/* ---------- 定时器回调函数：切换图片 ---------- */
static void img_timer_cb(lv_timer_t *timer)
{
    auto_idx = (auto_idx + 1) % (sizeof(auto_imgs) / sizeof(auto_imgs[0]));
    lv_img_set_src(auto_img, auto_imgs[auto_idx]);
}

/* ---------- 主开关回调 - 控制 4 个 LED 全亮或全灭 ---------- */
static void sw_main_cb(lv_event_t *e)
{
    pthread_mutex_lock(&data_mutex);
    g_data.led_main = !g_data.led_main;
    pthread_mutex_unlock(&data_mutex);

    if (led_fd >= 0) {
        int state = g_data.led_main ? LED_ON : LED_OFF;
        ioctl(led_fd, LED1, state);
        ioctl(led_fd, LED2, state);
        ioctl(led_fd, LED3, state);
        ioctl(led_fd, LED4, state);
    }

    if (g_data.led_main) lv_obj_add_state(sw_main, LV_STATE_CHECKED);
    else lv_obj_clear_state(sw_main, LV_STATE_CHECKED);

    // 发送开关状态到服务器
    if (socket_fd >= 0) {
        const char *msg = g_data.led_main ? "开灯" : "关灯";
        send(socket_fd, msg, strlen(msg), 0);
        printf("发送灯光状态: %s\n", msg);
    }
}

/* ---------- 报警开关回调 - 控制蜂鸣器开启或关闭 ---------- */
static void sw_aux_cb(lv_event_t *e)
{
    pthread_mutex_lock(&data_mutex);
    g_data.led_aux = !g_data.led_aux;
    pthread_mutex_unlock(&data_mutex);

    if (beep_fd >= 0) {
        int state = g_data.led_aux ? BEEP_ON : BEEP_OFF;
        ioctl(beep_fd, state, 1);
    }

    if (g_data.led_aux) lv_obj_add_state(sw_aux, LV_STATE_CHECKED);
    else lv_obj_clear_state(sw_aux, LV_STATE_CHECKED);

    // 发送报警状态到服务器
    if (socket_fd >= 0) {
        const char *msg = g_data.led_aux ? "开启报警" : "关闭报警";
        send(socket_fd, msg, strlen(msg), 0);
        printf("发送报警状态: %s\n", msg);
    }
}

/* ---------- 接收线程：处理服务器指令 ---------- */
static void *recv_thread(void *arg)
{
    int fd = (int)(long)arg;
    char buf[32];
    int n;

    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        printf("收到服务器指令: %s\n", buf);

        // 将指令放入队列
        pthread_mutex_lock(&queue_mutex);
        if ((queue_head + 1) % QUEUE_SIZE != queue_tail) { // 检查队列是否未满
            strncpy(command_queue[queue_head], buf, sizeof(command_queue[queue_head]) - 1);
            command_queue[queue_head][sizeof(command_queue[queue_head]) - 1] = '\0';
            queue_head = (queue_head + 1) % QUEUE_SIZE;
        } else {
            queue_full = true;
            printf("指令队列已满，丢弃指令: %s\n", buf);
        }
        pthread_mutex_unlock(&queue_mutex);
    }

    printf("服务器断开连接，退出接收线程\n");
    return NULL;
}

/* ---------- 后台线程：每 5 秒更新并发送数据 ---------- */
static void *refresh_thread(void *arg)
{
    while (!quit_refresh) {
        pthread_mutex_lock(&data_mutex);
        g_data.temp  = 20.0f + (rand() % 100) / 10.0f; // 温度: 20.0-29.9°C
        g_data.humi  = 60   + (rand() % 20);           // 湿度: 60-79%
        g_data.co2   = 400  + (rand() % 1600);         // CO2: 400-2000 ppm
        g_data.lux   = 5000 + (rand() % 5000);         // 光照度: 5000-10000 lux
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        snprintf(g_data.time, sizeof(g_data.time), "%02d:%02d",
                 tm_now.tm_hour, tm_now.tm_min);

        // 仅采集前 5 个点到折线图
        if (g_data.sample_count < 5) {
            g_data.co2_samples[g_data.sample_count] = (lv_coord_t)g_data.co2;
            g_data.lux_samples[g_data.sample_count] = (lv_coord_t)g_data.lux;
            g_data.temp_samples[g_data.sample_count] = (lv_coord_t)(g_data.temp * 10);
            g_data.humi_samples[g_data.sample_count] = (lv_coord_t)g_data.humi;
            g_data.sample_count++;
        }

        // 构造要发送的数据字符串
        char buf[256];
        snprintf(buf, sizeof(buf), 
                 "温度:%.1f °C  湿度:%.0f %%   CO2:%d ppm   光照度:%d lux   时间:%s   灯开关:%d   报警开关:%d",
                 g_data.temp, g_data.humi, g_data.co2, g_data.lux, g_data.time,
                 g_data.led_main ? 1 : 0, g_data.led_aux ? 1 : 0);

        // 通过 TCP 发送数据
        if (socket_fd >= 0) {
            int ret = send(socket_fd, buf, strlen(buf), 0);
            if (ret < 0) {
                perror("发送数据失败");
            } else {
                printf("发送数据: %s\n", buf);
            }
        }

        // printf("Refresh thread: temp=%.1f, humi=%.0f, co2=%d, lux=%d, time=%s\n",
        //        g_data.temp, g_data.humi, g_data.co2, g_data.lux, g_data.time);
        pthread_mutex_unlock(&data_mutex);
        sleep(5);
    }
    return NULL;
}

/* ---------- 界面创建（主线程） ---------- */
void nongye_ui_create(void)
{
    srand(time(NULL));

    if (!lv_scr_act()) {
        puts("!!! 屏幕未就绪，跳过 nongye_ui_create");
        return;
    }

    /* 打开 LED 设备文件 */
    led_fd = open("/dev/Led", O_RDWR);
    if (led_fd < 0) {
        perror("无法打开 /dev/Led");
    }

    /* 打开蜂鸣器设备文件 */
    beep_fd = open("/dev/beep", O_RDWR);
    if (beep_fd < 0) {
        perror("无法打开 /dev/beep");
    }

    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f2027), 0);

    /* 标题栏 */
    lv_obj_t *header = lv_obj_create(lv_scr_act());
    lv_obj_set_size(header, 800, 50);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x10b981), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题栏中间标题 */
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "智慧大棚监测系统");
    lv_obj_set_style_text_font(title, &chinese_ziku, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, -10);

    /* 标题栏左侧时间 */
    lab_time_header = lv_label_create(header);
    lv_label_set_text(lab_time_header, "2025-09-10 16:20");
    lv_obj_set_style_text_font(lab_time_header, &chinese_ziku, 0);
    lv_obj_align(lab_time_header, LV_ALIGN_TOP_LEFT, 20, -10);
    lv_obj_set_style_text_align(lab_time_header, LV_TEXT_ALIGN_LEFT, 0);

    /* 标题栏右侧天气 */
    lab_weather_header = lv_label_create(header);
    lv_label_set_text(lab_weather_header, "广州 晴天 30度");
    lv_obj_set_style_text_font(lab_weather_header, &chinese_ziku, 0);
    lv_obj_align(lab_weather_header, LV_ALIGN_TOP_RIGHT, -20, -10);
    lv_obj_set_style_text_align(lab_weather_header, LV_TEXT_ALIGN_RIGHT, 0);

    /* 主网格 */
    lv_obj_t *grid = lv_obj_create(lv_scr_act());
    lv_obj_set_size(grid, 800, 430);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(grid, 10, 0);

    /* LED 控制卡片 */
    lv_obj_t *card6 = lv_obj_create(grid);
    lv_obj_set_size(card6, 250, 200);
    lv_obj_set_style_bg_color(card6, lv_color_hex(0x3399cc), 0);
    lv_obj_add_flag(card6, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card6, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *label6 = lv_label_create(card6);
    lv_label_set_text(label6, "照明报警系统");
    lv_obj_set_style_text_font(label6, &chinese_ziku, 0);
    lv_obj_align(label6, LV_ALIGN_TOP_LEFT, 10, 10);
    sw_main = lv_switch_create(card6);
    lv_obj_set_size(sw_main, 60, 30);
    lv_obj_add_flag(sw_main, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sw_main, LV_ALIGN_CENTER, -50, 60);
    lv_obj_add_event_cb(sw_main, sw_main_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g_data.led_main) lv_obj_add_state(sw_main, LV_STATE_CHECKED);
    lv_obj_t *label_main = lv_label_create(card6);
    lv_label_set_text(label_main, "灯光");
    lv_obj_set_style_text_font(label_main, &chinese_ziku, 0);
    lv_obj_align_to(label_main, sw_main, LV_ALIGN_OUT_TOP_MID, 0, -5);
    sw_aux = lv_switch_create(card6);
    lv_obj_set_size(sw_aux, 60, 30);
    lv_obj_add_flag(sw_aux, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sw_aux, LV_ALIGN_CENTER, 50, 60);
    lv_obj_add_event_cb(sw_aux, sw_aux_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g_data.led_aux) lv_obj_add_state(sw_aux, LV_STATE_CHECKED);
    lv_obj_t *label_aux = lv_label_create(card6);
    lv_label_set_text(label_aux, "报警");
    lv_obj_set_style_text_font(label_aux, &chinese_ziku, 0);
    lv_obj_align_to(label_aux, sw_aux, LV_ALIGN_OUT_TOP_MID, 0, -5);

    /* 气体监测卡片 */
    lv_obj_t *card4 = lv_obj_create(grid);
    lv_obj_set_size(card4, 250, 200);
    lv_obj_set_style_bg_color(card4, lv_color_hex(0x3399cc), 0);
    lv_obj_t *label4 = lv_label_create(card4);
    lv_label_set_text(label4, "气体监测");
    lv_obj_set_style_text_font(label4, &chinese_ziku, 0);
    lv_obj_align(label4, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_t *label_co2_name = lv_label_create(card4);
    lv_label_set_text(label_co2_name, "CO2浓度");
    lv_obj_set_style_text_font(label_co2_name, &chinese_ziku, 0);
    lv_obj_align(label_co2_name, LV_ALIGN_TOP_MID, -60, 70);
    lab_co2 = lv_label_create(card4);
    lv_label_set_text_fmt(lab_co2, "%d ppm", g_data.co2);
    lv_obj_align(lab_co2, LV_ALIGN_TOP_MID, -60, 100);
    lv_obj_t *label_lux_name = lv_label_create(card4);
    lv_label_set_text(label_lux_name, "光照度");
    lv_obj_set_style_text_font(label_lux_name, &chinese_ziku, 0);
    lv_obj_align(label_lux_name, LV_ALIGN_TOP_MID, 60, 70);
    lab_lux = lv_label_create(card4);
    lv_label_set_text_fmt(lab_lux, "%d lux", g_data.lux);
    lv_obj_align(lab_lux, LV_ALIGN_TOP_MID, 60, 100);

    bar_co2 = lv_bar_create(card4);
    lv_obj_set_size(bar_co2, 200, 10);
    lv_obj_align(bar_co2, LV_ALIGN_CENTER, 0, 60);
    lv_bar_set_range(bar_co2, 0, 100);
    lv_bar_set_value(bar_co2, (g_data.co2 - 400) * 100 / 1600, LV_ANIM_OFF);

    /* 气体检测折线图卡片 */
    lv_obj_t *card3 = lv_obj_create(grid);
    lv_obj_set_size(card3, 250, 200);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x3399cc), 0);
    lv_obj_t *label3 = lv_label_create(card3);
    lv_label_set_text(label3, "气体检测趋势");
    lv_obj_set_style_text_font(label3, &chinese_ziku, 0);
    lv_obj_align(label3, LV_ALIGN_TOP_LEFT, 10, 10);

    chart_gas = lv_chart_create(card3);
    lv_obj_set_size(chart_gas, 200, 120);
    lv_obj_align(chart_gas, LV_ALIGN_CENTER, 0, 20);
    lv_chart_set_type(chart_gas, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_gas, 5);
    lv_chart_set_range(chart_gas, LV_CHART_AXIS_PRIMARY_Y, 0, 11000);
    lv_chart_set_range(chart_gas, LV_CHART_AXIS_SECONDARY_Y, 0, 7000);
    lv_chart_set_div_line_count(chart_gas, 5, 5);

    lv_chart_series_t *ser_co2 = lv_chart_add_series(chart_gas, lv_color_hex(0xFF0000), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *ser_lux = lv_chart_add_series(chart_gas, lv_color_hex(0x00FF00), LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_value(chart_gas, ser_co2, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart_gas, ser_lux, LV_CHART_POINT_NONE);

    lv_obj_t *label_co2_axis = lv_label_create(card3);
    lv_label_set_text(label_co2_axis, "CO2 (ppm)");
    lv_obj_set_style_text_color(label_co2_axis, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_text_font(label_co2_axis, &chinese_ziku, 0);
    lv_obj_align_to(label_co2_axis, chart_gas, LV_ALIGN_OUT_RIGHT_MID, 30, 15);

    lv_obj_t *label_lux_axis = lv_label_create(card3);
    lv_label_set_text(label_lux_axis, "光照 (lux)");
    lv_obj_set_style_text_color(label_lux_axis, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(label_lux_axis, &chinese_ziku, 0);
    lv_obj_align_to(label_lux_axis, chart_gas, LV_ALIGN_OUT_RIGHT_MID, 30, -15);

    /* 农业资讯卡片 */
    lv_obj_t *card2 = lv_obj_create(grid);
    lv_obj_set_size(card2, 250, 200);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x3399cc), 0);
    lv_obj_t *label2 = lv_label_create(card2);
    lv_label_set_text(label2, "农业资讯");
    lv_obj_set_style_text_font(label2, &chinese_ziku, 0);
    lv_obj_align(label2, LV_ALIGN_TOP_LEFT, 0, -15);
    auto_img = lv_img_create(card2);
    lv_obj_set_size(auto_img, 230, 160);
    lv_obj_align(auto_img, LV_ALIGN_BOTTOM_MID, 0, 16);
    lv_img_set_src(auto_img, auto_imgs[auto_idx]);
    img_timer = lv_timer_create(img_timer_cb, 3000, NULL);

    /* 温湿度监测卡片 */
    lv_obj_t *card1 = lv_obj_create(grid);
    lv_obj_set_size(card1, 250, 200);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x3399cc), 0);
    lv_obj_t *label1 = lv_label_create(card1);
    lv_label_set_text(label1, "温湿度监测");
    lv_obj_set_style_text_font(label1, &chinese_ziku, 0);
    lv_obj_align(label1, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_t *label_temp_name = lv_label_create(card1);
    lv_label_set_text(label_temp_name, "温度");
    lv_obj_set_style_text_font(label_temp_name, &chinese_ziku, 0);
    lv_obj_align(label_temp_name, LV_ALIGN_CENTER, -50, 10);
    lab_temp = lv_label_create(card1);
    lv_label_set_text_fmt(lab_temp, "%.1f°C", g_data.temp);
    lv_obj_align(lab_temp, LV_ALIGN_CENTER, -50, 40);
    lv_obj_t *label_humi_name = lv_label_create(card1);
    lv_label_set_text(label_humi_name, "湿度");
    lv_obj_set_style_text_font(label_humi_name, &chinese_ziku, 0);
    lv_obj_align(label_humi_name, LV_ALIGN_CENTER, 50, 10);
    lab_humi = lv_label_create(card1);
    lv_label_set_text_fmt(lab_humi, "%.0f%%", g_data.humi);
    lv_obj_align(lab_humi, LV_ALIGN_CENTER, 50, 40);

    /* 温湿度趋势折线图卡片 */
    lv_obj_t *card5 = lv_obj_create(grid);
    lv_obj_set_size(card5, 250, 200);
    lv_obj_set_style_bg_color(card5, lv_color_hex(0x3399cc), 0);
    lv_obj_t *label5 = lv_label_create(card5);
    lv_label_set_text(label5, "温湿度趋势");
    lv_obj_set_style_text_font(label5, &chinese_ziku, 0);
    lv_obj_align(label5, LV_ALIGN_TOP_LEFT, 10, 10);

    chart_env = lv_chart_create(card5);
    lv_obj_set_size(chart_env, 200, 120);
    lv_obj_align(chart_env, LV_ALIGN_CENTER, 0, 20);
    lv_chart_set_type(chart_env, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_env, 5);
    lv_chart_set_range(chart_env, LV_CHART_AXIS_PRIMARY_Y, 0, 200);
    lv_chart_set_range(chart_env, LV_CHART_AXIS_SECONDARY_Y, 0, 400);
    lv_chart_set_div_line_count(chart_env, 5, 5);

    lv_chart_series_t *ser_temp = lv_chart_add_series(chart_env, lv_color_hex(0xFF0000), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *ser_humi = lv_chart_add_series(chart_env, lv_color_hex(0x00FF00), LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_value(chart_env, ser_temp, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart_env, ser_humi, LV_CHART_POINT_NONE);

    lv_obj_t *label_temp_axis = lv_label_create(card5);
    lv_label_set_text(label_temp_axis, "温度");
    lv_obj_set_style_text_color(label_temp_axis, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(label_temp_axis, &chinese_ziku, 0);
    lv_obj_align_to(label_temp_axis, chart_env, LV_ALIGN_OUT_RIGHT_MID, 30, 15);

    lv_obj_t *label_humi_axis = lv_label_create(card5);
    lv_label_set_text(label_humi_axis, "湿度");
    lv_obj_set_style_text_color(label_humi_axis, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_text_font(label_humi_axis, &chinese_ziku, 0);
    lv_obj_align_to(label_humi_axis, chart_env, LV_ALIGN_OUT_RIGHT_MID, 30, -15);

    /* 启动后台刷新线程 */
    pthread_create(&refresh_tid, NULL, refresh_thread, NULL);
}

/* ---------- 主线程周期刷新（带 NULL 保护） ---------- */
void nongye_ui_refresh(void)
{
    if (!lab_temp || !lab_humi || !lab_co2 || !lab_lux || !sw_main || !sw_aux || !bar_co2 || !lab_time_header || !chart_gas || !chart_env) {
        printf("Error: One or more UI objects are NULL in nongye_ui_refresh\n");
        return;
    }

    pthread_mutex_lock(&data_mutex);
    float temp = g_data.temp;
    float humi = g_data.humi;
    int co2 = g_data.co2;
    int lux = g_data.lux;
    char time_buf[8];
    strncpy(time_buf, g_data.time, sizeof(time_buf));
    pthread_mutex_unlock(&data_mutex);

    // 处理指令队列
    pthread_mutex_lock(&queue_mutex);
    while (queue_tail != queue_head) {
        char *cmd = command_queue[queue_tail];
        pthread_mutex_lock(&data_mutex);
        if (strcmp(cmd, "开灯") == 0) {
            g_data.led_main = true;
            if (led_fd >= 0) {
                ioctl(led_fd, LED1, LED_ON);
                ioctl(led_fd, LED2, LED_ON);
                ioctl(led_fd, LED3, LED_ON);
                ioctl(led_fd, LED4, LED_ON);
            }
            if (sw_main) {
                lv_obj_add_state(sw_main, LV_STATE_CHECKED);
            }
        } else if (strcmp(cmd, "关灯") == 0) {
            g_data.led_main = false;
            if (led_fd >= 0) {
                ioctl(led_fd, LED1, LED_OFF);
                ioctl(led_fd, LED2, LED_OFF);
                ioctl(led_fd, LED3, LED_OFF);
                ioctl(led_fd, LED4, LED_OFF);
            }
            if (sw_main) {
                lv_obj_clear_state(sw_main, LV_STATE_CHECKED);
            }
        } else if (strcmp(cmd, "开启报警") == 0) {
            g_data.led_aux = true;
            if (beep_fd >= 0) {
                ioctl(beep_fd, BEEP_ON, 1);
            }
            if (sw_aux) {
                lv_obj_add_state(sw_aux, LV_STATE_CHECKED);
            }
        } else if (strcmp(cmd, "关闭报警") == 0) {
            g_data.led_aux = false;
            if (beep_fd >= 0) {
                ioctl(beep_fd, BEEP_OFF, 1);
            }
            if (sw_aux) {
                lv_obj_clear_state(sw_aux, LV_STATE_CHECKED);
            }
        }
        pthread_mutex_unlock(&data_mutex);
        queue_tail = (queue_tail + 1) % QUEUE_SIZE;
    }
    queue_full = false;
    pthread_mutex_unlock(&queue_mutex);

    // 更新 UI
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char date_time_buf[17];
    snprintf(date_time_buf, sizeof(date_time_buf), "%04d-%02d-%02d %02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min);
    lv_label_set_text(lab_time_header, date_time_buf);

    lv_label_set_text_fmt(lab_temp, "%.1f°C", temp);
    lv_label_set_text_fmt(lab_humi, "%.0f%%", humi);
    lv_label_set_text_fmt(lab_co2, "%d ppm", co2);
    lv_label_set_text_fmt(lab_lux, "%d lux", lux);
    lv_bar_set_value(bar_co2, (co2 - 400) * 100 / 1600, LV_ANIM_OFF);

    if (g_data.sample_count > 0) {
        lv_chart_series_t *ser_co2 = lv_chart_get_series_next(chart_gas, NULL);
        lv_chart_series_t *ser_lux = lv_chart_get_series_next(chart_gas, ser_co2);
        lv_chart_set_ext_y_array(chart_gas, ser_co2, g_data.co2_samples);
        lv_chart_set_ext_y_array(chart_gas, ser_lux, g_data.lux_samples);
        lv_chart_refresh(chart_gas);

        lv_chart_series_t *ser_temp = lv_chart_get_series_next(chart_env, NULL);
        lv_chart_series_t *ser_humi = lv_chart_get_series_next(chart_env, ser_temp);
        lv_chart_set_ext_y_array(chart_env, ser_temp, g_data.temp_samples);
        lv_chart_set_ext_y_array(chart_env, ser_humi, g_data.humi_samples);
        lv_chart_refresh(chart_env);
    }
}

/* ---------- 清理资源 ---------- */
void nongye_ui_cleanup(void)
{
    quit_refresh = true;
    pthread_join(refresh_tid, NULL);
    pthread_join(recv_tid, NULL);
    if (led_fd >= 0) {
        close(led_fd);
        led_fd = -1;
    }
    if (beep_fd >= 0) {
        close(beep_fd);
        beep_fd = -1;
    }
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    chart_gas = NULL;
    chart_env = NULL;
    pthread_mutex_destroy(&data_mutex);
    pthread_mutex_destroy(&queue_mutex);
    printf("资源清理完成\n");
}

/* ---------- 信号处理函数 ---------- */
static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        printf("收到退出信号，清理资源...\n");
        nongye_ui_cleanup();
        exit(0);
    }
}

/* ---------- 初始化信号处理和 TCP 客户端 ---------- */
void nongye_init(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("创建套接字失败");
        return;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)) < 0) {
        perror("连接服务器失败");
        close(socket_fd);
        socket_fd = -1;
        return;
    }
    printf("连接服务器成功 [%s:%d]\n", SERVER_IP, SERVER_PORT);

    // 启动接收线程
    pthread_create(&recv_tid, NULL, recv_thread, (void *)(long)socket_fd);
}