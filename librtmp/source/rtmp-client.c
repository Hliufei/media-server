/* Adobe��s Real Time Messaging Protocol

1. handshake (p10)
C -> S: C0/C1
S -> C: S0/S1/S2
C -> S: C2

2. connect (p34)
C -> S: connect
S -> C: Window Acknowledgement Size
S -> C: Set Peer Bandwidth
C -> S: Window Acknowledgement Size
S -> C: User Control Message (StreamBegin)
S -> C: _result(connect response)

3. play (p41)
C -> S: createStream
S -> C: _result(createStream response)
C -> S: play
C -> S: SetBufferLength (optional)
S -> C: SetChunkSize
S -> C: User Control (StreamIsRecorded)
S -> C: User Control (StreamBegin)
S -> C: onStatus (play reset) (only play set reset flag)
S -> C: onStatus (play start)
S -> C: Audio
S -> C: Video

4. publish (p49)
C -> S: createStream
S -> C: _result(createStream response)
C -> S: publish
S -> C: User Control (StreamBegin)
C -> S: Metadata
C -> S: Audio
C -> S: SetChunkSize
S -> C: onStatus(publish result)
C -> S: Video
*/

#include "rtmp-client.h"
#include "rtmp-internal.h"
#include "rtmp-msgtypeid.h"
#include "rtmp-handshake.h"
#include "rtmp-control-message.h"
#include "rtmp-netconnection.h"
#include "rtmp-netstream.h"
#include "rtmp-event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#define FLASHVER "LNX 9,0,124,2"

struct rtmp_client_t
{
	struct rtmp_t rtmp;
	struct rtmp_connect_t connect;

	uint32_t stream_id; // createStream/deleteStream
	char stream_name[256]; // Play/Publishing stream name, flv:sample, mp3:sample, H.264/AAC: mp4:sample.m4v
	enum rtmp_state_t state;

	struct rtmp_client_handler_t handler;
	void* param;

	uint8_t payload[2*1024];
	//uint8_t handshake[RTMP_HANDSHAKE_SIZE + 1]; // only for handshake
	size_t handshake_bytes;
	int handshake_state; // RTMP_HANDSHAKE_XXX

	int publish; // 0-publish, 1-live/vod, 2-live only, 3-vod only
};

static int rtmp_client_send_control(struct rtmp_t* rtmp, const uint8_t* payload, uint32_t bytes, uint32_t stream_id)
{
	struct rtmp_chunk_header_t header;
	header.fmt = RTMP_CHUNK_TYPE_0; // disable compact header
	header.cid = RTMP_CHANNEL_INVOKE;
	header.timestamp = 0;
	header.length = bytes;
	header.type = RTMP_TYPE_INVOKE;
	header.stream_id = stream_id; /* default 0 */
	return rtmp_chunk_write(rtmp, &header, payload);
}

// C2
static int rtmp_client_send_c2(struct rtmp_client_t* ctx)
{
	int r;
	rtmp_handshake_c2(ctx->payload, (uint32_t)time(NULL), ctx->payload, RTMP_HANDSHAKE_SIZE);
	r = ctx->handler.send(ctx->param, ctx->payload, RTMP_HANDSHAKE_SIZE);
	return RTMP_HANDSHAKE_SIZE == r ? 0 : -1;
}

// Connect
static int rtmp_client_send_connect(struct rtmp_client_t* ctx)
{
	int r;
	r = rtmp_netconnection_connect(ctx->payload, sizeof(ctx->payload), RTMP_TRANSACTION_CONNECT, &ctx->connect) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, 0);
}

// ReleaseStream
static int rmtp_client_send_release_stream(struct rtmp_client_t* ctx)
{
	int r;
	r = rtmp_netstream_release_stream(ctx->payload, sizeof(ctx->payload), 0, ctx->stream_name) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

// FCPublish
static int rtmp_client_send_fcpublish(struct rtmp_client_t* ctx)
{
	int r;
	r = rtmp_netstream_fcpublish(ctx->payload, sizeof(ctx->payload), 0, ctx->stream_name) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, 0);
}

// FCUnpublish
static int rtmp_client_send_fcunpublish(struct rtmp_client_t* ctx)
{
	int r;
	r = rtmp_netstream_fcunpublish(ctx->payload, sizeof(ctx->payload), 0, ctx->stream_name) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

// createStream
static int rtmp_client_send_create_stream(struct rtmp_client_t* ctx)
{
	int r;
	assert(0 == ctx->stream_id);
	r = rtmp_netconnection_create_stream(ctx->payload, sizeof(ctx->payload), RTMP_TRANSACTION_CREATE_STREAM) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, 0);
}

// deleteStream
static int rtmp_client_send_delete_stream(struct rtmp_client_t* ctx)
{
	int r;
	assert(0 != ctx->stream_id);
	r = rtmp_netstream_delete_stream(ctx->payload, sizeof(ctx->payload), 0, ctx->stream_id) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

// publish
static int rtmp_client_send_publish(struct rtmp_client_t* ctx)
{
	int r;
	assert(0 != ctx->stream_id);
	r = rtmp_netstream_publish(ctx->payload, sizeof(ctx->payload), 0, ctx->stream_name, RTMP_STREAM_LIVE) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

// play
static int rtmp_client_send_play(struct rtmp_client_t* ctx)
{
	int r;
	assert(0 != ctx->stream_id);
	r = rtmp_netstream_play(ctx->payload, sizeof(ctx->payload), 0, ctx->stream_name, -2, -1, 1) - ctx->payload;
//	rtmp_client_chunk_header_default(&header, RTMP_CHANNEL_CONTROL, (uint32_t)time(NULL), r, RTMP_TYPE_INVOKE, ctx->stream_id);
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

/// 5.4.1. Set Chunk Size (1)
static int rtmp_client_send_set_chunk_size(struct rtmp_client_t* ctx)
{
	int n, r;
	assert(0 == ctx->publish);
	n = rtmp_set_chunk_size(ctx->payload, sizeof(ctx->payload), ctx->rtmp.out_chunk_size);
	r = ctx->handler.send(ctx->param, ctx->payload, n);
	return n == r ? 0 : r;
}

// Window Acknowledgement Size (5)
static int rtmp_client_send_server_bandwidth(struct rtmp_client_t* ctx)
{
	int n, r;
	n = rtmp_window_acknowledgement_size(ctx->payload, sizeof(ctx->payload), ctx->rtmp.window_size);
	r = ctx->handler.send(ctx->param, ctx->payload, n);
	return n == r ? 0 : r;
}

static int rtmp_client_send_set_buffer_length(struct rtmp_client_t* ctx)
{
	int n, r;
	n = rtmp_event_set_buffer_length(ctx->payload, sizeof(ctx->payload), ctx->stream_id, ctx->rtmp.buffer_length_ms);
	r = ctx->handler.send(ctx->param, ctx->payload, n);
	return n == r ? 0 : r;
}

static int rtmp_client_send_get_stream_length(struct rtmp_client_t* ctx)
{
	int r;
	r = rtmp_netconnection_get_stream_length(ctx->payload, sizeof(ctx->payload), RTMP_TRANSACTION_GET_STREAM_LENGTH, ctx->stream_name) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

static int rtmp_client_onconnect(void* param)
{
	int r = 0;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;
	ctx->state = RTMP_STATE_CONNECTED;
	if (0 == ctx->publish)
	{
		// publish only
		r = rmtp_client_send_release_stream(ctx);
		r = 0 == r ? rtmp_client_send_fcpublish(ctx) : r;
	}
	else
	{
		r = rtmp_client_send_server_bandwidth(ctx);
	}

	return 0 == r ? rtmp_client_send_create_stream(ctx) : r;
}

static int rtmp_client_oncreate_stream(void* param, double stream_id)
{
	int r = 0;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;
	ctx->state = RTMP_STATE_CREATE_STREAM;
	ctx->stream_id = (uint32_t)stream_id;

	if (0 == ctx->publish)
	{
		r = rtmp_client_send_publish(ctx);
		r = 0 == r ? rtmp_client_send_set_chunk_size(ctx) : r;
	}
	else
	{
		if (3 == ctx->publish)
			r = rtmp_client_send_get_stream_length(ctx);
		r = 0 == r ? rtmp_client_send_play(ctx) : r;
		r = 0 == r ? rtmp_client_send_set_buffer_length(ctx) : r;
	}

	return r;
}

static int rtmp_client_onnotify(void* param, enum rtmp_notify_t notify)
{
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;

	switch (notify)
	{
	case RTMP_NOTIFY_START:
		ctx->state = RTMP_STATE_START;
		break;

	case RTMP_NOTIFY_STOP:
		ctx->state = RTMP_STATE_STOP;
		break;

	case RTMP_NOTIFY_PAUSE:
	case RTMP_NOTIFY_SEEK:
	default:
		break;
	}

	return 0;
}

static int rtmp_client_onping(void* param, uint32_t seqNo)
{
	int n, r;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;

	n = rtmp_event_pong(ctx->payload, sizeof(ctx->payload), seqNo);
	r = ctx->handler.send(ctx->param, ctx->payload, n);
	return n == r ? 0 : r;
}

static int rtmp_client_onbandwidth(void* param)
{
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;
	return rtmp_client_send_server_bandwidth(ctx);
}

static void rtmp_client_onabort(void* param, uint32_t chunk_stream_id)
{
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;
	(void)chunk_stream_id;
}

static int rtmp_client_onaudio(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;
	return ctx->handler.onaudio(ctx->param, data, bytes, timestamp);
}

static int rtmp_client_onvideo(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)param;
	return ctx->handler.onvideo(ctx->param, data, bytes, timestamp);
}

static int rtmp_client_send(void* param, const uint8_t* header, uint32_t headerBytes, const uint8_t* payload, uint32_t payloadBytes)
{
	int r;
	struct rtmp_client_t* ctx;
	//static uint8_t s_payload[1024 * 2];
	ctx = (struct rtmp_client_t*)param;
	//memcpy(s_payload, header, headerBytes);
	//memcpy(s_payload + headerBytes, payload, payloadBytes);
	//r = ctx->handler.send(ctx->param, s_payload, headerBytes + payloadBytes);
	r = ctx->handler.send(ctx->param, header, headerBytes);
	if ((int)headerBytes == r && payloadBytes > 0)
		r = ctx->handler.send(ctx->param, payload, payloadBytes);
	return (r == (int)(payloadBytes > 0 ? payloadBytes : headerBytes)) ? 0 : -1;
}

void* rtmp_client_create(const char* appname, const char* playpath, const char* tcurl, void* param, const struct rtmp_client_handler_t* handler)
{
	struct rtmp_client_t* ctx;

	assert(appname && *appname && playpath && *playpath && handler);
	ctx = (struct rtmp_client_t*)malloc(sizeof(struct rtmp_client_t));
	if (!ctx) return NULL;

	memset(ctx, 0, sizeof(struct rtmp_client_t));
	memcpy(&ctx->handler, handler, sizeof(ctx->handler));
	strlcpy(ctx->stream_name, playpath, sizeof(ctx->stream_name));
	ctx->stream_id = 0;
	ctx->param = param;
	ctx->state = RTMP_STATE_UNINIT;

	ctx->rtmp.parser.state = RTMP_PARSE_INIT;
	ctx->rtmp.in_chunk_size = RTMP_CHUNK_SIZE;
	ctx->rtmp.out_chunk_size = RTMP_CHUNK_SIZE;
	ctx->rtmp.window_size = 2500000;
	ctx->rtmp.peer_bandwidth = 2500000;
	ctx->rtmp.buffer_length_ms = 30000;

	ctx->rtmp.param = ctx;
	ctx->rtmp.send = rtmp_client_send;
	ctx->rtmp.onaudio = rtmp_client_onaudio;
	ctx->rtmp.onvideo = rtmp_client_onvideo;
	ctx->rtmp.onabort = rtmp_client_onabort;
	ctx->rtmp.u.client.onconnect = rtmp_client_onconnect;
	ctx->rtmp.u.client.oncreate_stream = rtmp_client_oncreate_stream;
	ctx->rtmp.u.client.onnotify = rtmp_client_onnotify;
	ctx->rtmp.u.client.onping = rtmp_client_onping;
	ctx->rtmp.u.client.onbandwidth = rtmp_client_onbandwidth;

	strlcpy(ctx->connect.app, appname, sizeof(ctx->connect.app));
	if (tcurl) strlcpy(ctx->connect.tcUrl, tcurl, sizeof(ctx->connect.tcUrl));
	//strlcpy(ctx->connect.swfUrl, tcurl ? tcurl : url, sizeof(ctx->connect.swfUrl));
	//strlcpy(ctx->connect.pageUrl, tcurl ? tcurl : url, sizeof(ctx->connect.pageUrl));
	strlcpy(ctx->connect.flashver, FLASHVER, sizeof(ctx->connect.flashver));
	ctx->connect.fpad = 0;
	ctx->connect.capabilities = 15;
	ctx->connect.audioCodecs = 3191; //SUPPORT_SND_AAC;
	ctx->connect.videoCodecs = 252; // SUPPORT_VID_H264;
	ctx->connect.videoFunction = SUPPORT_VID_CLIENT_SEEK;

	return ctx;
}

void rtmp_client_destroy(void* client)
{
	size_t i;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;
	assert(sizeof(ctx->rtmp.in_packets) == sizeof(ctx->rtmp.out_packets));
	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		assert(NULL == ctx->rtmp.out_packets[i].payload);
		if (ctx->rtmp.in_packets[i].payload)
			free(ctx->rtmp.in_packets[i].payload);
	}

	free(ctx);
}

int rtmp_client_input(void* client, const void* data, size_t bytes)
{
	int r;
	const uint8_t* p;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;

	p = data;
	while (bytes > 0)
	{
		switch (ctx->handshake_state)
		{
		case RTMP_HANDSHAKE_UNINIT: // S0: version
			ctx->handshake_state = RTMP_HANDSHAKE_0;
			ctx->handshake_bytes = 0; // clear buffer
			assert(*p <= RTMP_VERSION);
			bytes -= 1;
			p += 1;
			break;

		case RTMP_HANDSHAKE_0: // S1: 4-time + 4-zero + 1528-random
			if (bytes + ctx->handshake_bytes < RTMP_HANDSHAKE_SIZE)
			{
				memcpy(ctx->payload + ctx->handshake_bytes, p, bytes);
				ctx->handshake_bytes += bytes;
				p += bytes;
				bytes = 0; // 0
			}
			else
			{
				memcpy(ctx->payload + ctx->handshake_bytes, p, RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes);
				bytes -= RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes;
				p += RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes;
				ctx->handshake_state = RTMP_HANDSHAKE_1;
				ctx->handshake_bytes = 0; // clear buffer
				r = rtmp_client_send_c2(ctx);
				if(0 != r) return r;
			}
			break;

		case RTMP_HANDSHAKE_1: // S2: 4-time + 4-time2 + 1528-echo
			if (bytes + ctx->handshake_bytes < RTMP_HANDSHAKE_SIZE)
			{
				memcpy(ctx->payload + ctx->handshake_bytes, p, bytes);
				ctx->handshake_bytes += bytes;
				p += bytes;
				bytes = 0; // 0
			}
			else
			{
				memcpy(ctx->payload + ctx->handshake_bytes, p, RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes);
				bytes -= RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes;
				p += RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes;
				ctx->handshake_state = RTMP_HANDSHAKE_2;
				ctx->handshake_bytes = 0; // clear buffer
				r = rtmp_client_send_connect(ctx);
				if (0 != r) return r;
			}
			break;

		case RTMP_HANDSHAKE_2:
			ctx->state = RTMP_STATE_HANDSHAKE;

		default:
			return rtmp_chunk_read(&ctx->rtmp, (const uint8_t*)data, bytes);
		}
	}

	return 0; // need more data
}

int rtmp_client_start(void* client, int publish)
{
	int n;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;
	ctx->publish = publish;

	// handshake C0/C1
	ctx->handshake_state = RTMP_HANDSHAKE_UNINIT;
	n = rtmp_handshake_c0(ctx->payload, RTMP_VERSION);
	n += rtmp_handshake_c1(ctx->payload + n, (uint32_t)time(NULL));
	assert(n == RTMP_HANDSHAKE_SIZE + 1);
	return n == ctx->handler.send(ctx->param, ctx->payload, n) ? 0 : -1;
}

int rtmp_client_stop(void* client)
{
	int r = 0;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;

	if (0 == ctx->publish)
	{
		r = rtmp_client_send_fcunpublish(ctx);
	}

	return 0 == r ? rtmp_client_send_delete_stream(ctx) : r;
}

int rtmp_client_pause(void* client, int pause)
{
	int r, i;
	uint32_t timestamp = 0;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;

	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		if(0 == ctx->rtmp.in_packets[i].header.cid)
			continue;
		if (timestamp < ctx->rtmp.in_packets[i].header.timestamp)
			timestamp = ctx->rtmp.in_packets[i].header.timestamp;
	}

	r = rtmp_netstream_pause(ctx->payload, sizeof(ctx->payload), 0, pause, timestamp) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

int rtmp_client_seek(void* client, double timestamp)
{
	int r;
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;

	r = rtmp_netstream_seek(ctx->payload, sizeof(ctx->payload), 0, timestamp) - ctx->payload;
	return rtmp_client_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

int rtmp_client_getstate(void* client)
{
	struct rtmp_client_t* ctx;
	ctx = (struct rtmp_client_t*)client;
	return ctx->state;
}

int rtmp_client_push_video(void* client, const void* video, size_t bytes, uint32_t timestamp)
{
	struct rtmp_client_t* ctx;
	struct rtmp_chunk_header_t header;
	ctx = (struct rtmp_client_t*)client;

	assert(0 != ctx->stream_id);
	header.fmt = RTMP_CHUNK_TYPE_1; // enable compact header
	header.cid = RTMP_CHANNEL_VIDEO;
	header.timestamp = timestamp;
	header.length = (uint32_t)bytes;
	header.type = RTMP_TYPE_VIDEO;
	header.stream_id = ctx->stream_id;

	return rtmp_chunk_write(&ctx->rtmp, &header, (const uint8_t*)video);
}

int rtmp_client_push_audio(void* client, const void* audio, size_t bytes, uint32_t timestamp)
{
	struct rtmp_client_t* ctx;
	struct rtmp_chunk_header_t header;
	ctx = (struct rtmp_client_t*)client;

	assert(0 != ctx->stream_id);
	header.fmt = RTMP_CHUNK_TYPE_1; // enable compact header
	header.cid = RTMP_CHANNEL_AUDIO;
	header.timestamp = timestamp;
	header.length = (uint32_t)bytes;
	header.type = RTMP_TYPE_AUDIO;
	header.stream_id = ctx->stream_id;

	return rtmp_chunk_write(&ctx->rtmp, &header, (const uint8_t*)audio);
}
