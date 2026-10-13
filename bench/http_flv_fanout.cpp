#include "bench/media_fanout.h"
#include "bench/clients/http_flv_client.h"

int main(int argc, char** argv) { return media_server::bench::media_fanout<media_server::bench::http_flv_client>::run(argc, argv, 8080); }
