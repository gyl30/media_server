#include "rtsp-server-internal.h"

#include <string.h>

// RFC 2326 10.1 OPTIONS (p30)
int rtsp_server_options(struct rtsp_server_t* rtsp, const char* uri)
{
	http_get_header_by_name(rtsp->parser, "Connection");
	http_get_header_by_name(rtsp->parser, "Require");
	http_get_header_by_name(rtsp->parser, "Proxy-Require");
	http_get_header_by_name(rtsp->parser, "Proxy-Authenticate");

	if (rtsp->handler.onoptions)
		return rtsp->handler.onoptions(rtsp->param, rtsp, uri);
	else
		return rtsp_server_reply_options(rtsp, 200);
}

int rtsp_server_reply_options(rtsp_server_t* rtsp, int code)
{
	char header[256] = "Public: OPTIONS";
	if (rtsp->handler.ondescribe) strcat(header, ",DESCRIBE");
	if (rtsp->handler.onsetup) strcat(header, ",SETUP");
	if (rtsp->handler.onteardown) strcat(header, ",TEARDOWN");
	if (rtsp->handler.onplay) strcat(header, ",PLAY");
	if (rtsp->handler.onpause) strcat(header, ",PAUSE");
	if (rtsp->handler.onannounce) strcat(header, ",ANNOUNCE");
	if (rtsp->handler.onrecord) strcat(header, ",RECORD");
	if (rtsp->handler.ongetparameter) strcat(header, ",GET_PARAMETER");
	if (rtsp->handler.onsetparameter) strcat(header, ",SET_PARAMETER");
	strcat(header, "\r\n");
	return rtsp_server_reply2(rtsp, code, header, NULL, 0);
}
