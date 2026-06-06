# 安装依赖
sudo apt install -y build-essential pkg-config libgstrtspserver-1.0-dev

# 拉源码
mkdir -p ~/work && cd ~/work
wget -O test-launch.c \
    https://gitlab.freedesktop.org/gstreamer/gstreamer/-/raw/main/subprojects/gst-rtsp-server/examples/test-launch.c

# 编译
gcc test-launch.c -o test-launch \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-rtsp-server-1.0)

 #  test-launch.c 核心逻辑
 # /* test-launch 把命令行字符串变成一个 RTSP factory 注册到 server 上 */
 ##include <gst_launch/gst_launch.h>
 ##include <gst_launch/rtsp-server/rtsp-server.h>
 #
 #int main(int argc, char *argv[]) {
 #    gst_init(&argc, &argv);
 #
 #    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
 #    GstRTSPServer *server = gst_rtsp_server_new();
 #    gst_rtsp_server_set_service(server, "8554");          // 监听端口
 #
 #    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
 #    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
 #
 #    /* 关键：launch 字符串就是 gst_launch-launch-1.0 那一长串
 #       区别是用括号包裹，并且最后一个 element 必须叫 pay0 / pay1 */
 #    gst_rtsp_media_factory_set_launch(factory, argv[1]);
 #
 #    /* 多客户端共用一个底层 pipeline，节省 CPU/编码器 */
 #    gst_rtsp_media_factory_set_shared(factory, TRUE);
 #
 #    gst_rtsp_mount_points_add_factory(mounts, "/test", factory);
 #    g_object_unref(mounts);
 #
 #    gst_rtsp_server_attach(server, NULL);                 // 接到 GMainLoop
 #    g_print("stream ready at rtsp://127.0.0.1:8554/test\n");
 #    g_main_loop_run(loop);
 #    return 0;
 #}