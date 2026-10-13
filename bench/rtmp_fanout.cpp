#include "bench/media_fanout.h"
#include "bench/clients/rtmp_client.h"

int main(int argc, char** argv) { return media_server::bench::media_fanout<media_server::bench::rtmp_client>::run(argc, argv, 1935); }
