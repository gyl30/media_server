#include "bench/media_fanout.h"
#include "bench/clients/rtsp_client.h"

int main(int argc, char** argv)
{
    return media_server::bench::media_fanout<media_server::bench::rtsp_client>::run(argc, argv, 8554);
}
