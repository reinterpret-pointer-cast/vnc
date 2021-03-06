﻿#include <WITCH/WITCH.h>
#include <WITCH/PR/PR.h>
#include <WITCH/T/T.h>
#include <WITCH/A/A.h>
#include <WITCH/VEC/VEC.h>
#include <WITCH/RAND/RAND.h>
#include <WITCH/VAS/VAS.h>
#include <WITCH/STR/STR.h>
#include <WITCH/IO/SCR/SCR.h>
#include <WITCH/IO/print.h>
#include <WITCH/EV/EV.h>
#include <WITCH/NET/TCP/TCP.h>
#include <WITCH/NET/TCP/TLS/TLS.h>
#include <WITCH/ETC/AV/AV.h>

#include <fan/graphics/graphics.hpp>

constexpr f_t font_size = 16;

struct base_t{
	EV_t listener;
	struct{
		struct{
			VAS_t server;
			NET_TCP_t *tcpview;
			NET_TCP_eid_t tcpview_secret_eid;
			NET_TCP_t *tcpgrabto;
			NET_TCP_eid_t tcpgrabto_secret_eid;
			NET_TCP_eid_t tcpgrabto_eid;
		}tcp;
	}net;
	struct gui{

		static constexpr fan::vec2 box_size{ 200, font_size };
		static constexpr fan::vec2 border_size{ 20, 10 };
		static constexpr fan::vec2 nowhere{ -10000, -10000 };

		gui() : window(fan::vec2(480, 320)), camera(&window), boxes(&camera, fan_2d::gui::e_text_position::middle), tr(&camera), stb(&camera, fan_2d::gui::e_text_position::left) { }

		fan::window window;
		fan::camera camera;

		fan_2d::gui::selectable_sized_text_box boxes;
		fan_2d::gui::text_renderer tr;
		fan_2d::gui::sized_text_box stb;

		f64_t line(uint32_t n){
			return font_size * n;
		}
		fan::vec2 button(uint32_t n){
			n++;
			fan::vec2ui bottom_center = window.get_size();
			bottom_center.x /= 2;
			fan::vec2ui r = bottom_center - fan::cast<uint_t>(fan::vec2(box_size.x / 2, (box_size.y * n) + n * (border_size.y + 1)));
			return r;
		}
		void formBegin(){
			stb.erase(0, stb.size());
			tr.erase(0, tr.size());
		}
		void formPush(const wchar_t *str0, const fan::color color0, const wchar_t *str1, const fan::color color1){
			tr.push_back(str0, 0, color0, font_size);
			stb.push_back(str1, font_size, 0, fan::vec2(300, line(1)), 0, color1);
		}
		void formEnd(fan::vec2 pos){
			f64_t longest = tr.get_longest_text() + 20;

			for (int j = 0; j < tr.size(); j++) {
				tr.set_position(j, stb.get_text_position(j, pos));
				stb.set_position(j, fan::vec2(pos.x + longest, pos.y));
				stb.set_input_callback(j);
				pos.y += stb.get_size(j).y + 1;
			}
		}

		EV_ev_t_t evt;
	}gui;
};

void init_tls(NET_TCP_t *tcp){
	NET_TCP_TLS_add(tcp, &TLS_generated_ctx);
}

uint32_t server_secret_connstate_cb(NET_TCP_peer_t *peer, uint64_t *secret, uint8_t *pd, uint8_t flag){
	if(!(flag & NET_TCP_connstate_succ_e))
		return 0;
	*pd = 0;
	return NET_TCP_EXT_dontgo_e;
}
uint32_t server_secret_read_cb(NET_TCP_peer_t *peer, uint64_t *secret, uint8_t *pd, uint8_t **data, uint_t *size){
	if(!*pd)do{
		if(*size < sizeof(*secret)){
			IO_print(FD_OUT, "[!] %08x%04x sent wrong sized secret\n", peer->sdstaddr.ip, peer->sdstaddr.port);
			return NET_TCP_EXT_abconn_e;
		}
		if(*(uint64_t *)*data != *secret){
			IO_print(FD_OUT, "[!] %08x%04x sent wrong secret\n", peer->sdstaddr.ip, peer->sdstaddr.port);
			return NET_TCP_EXT_abconn_e;
		}
		*pd = 1;
		peer->loff.connstate++;
		uint8_t flag = NET_TCP_EXT_signal_connstate(peer, NET_TCP_connstate_succ_e);
		if(flag & NET_TCP_EXT_abconn_e){
			NET_TCP_closehard(peer);
		}
		if(*size >= sizeof(*secret)){
			*size -= sizeof(*secret);
			*data += sizeof(*secret);
			break;
		}
		return NET_TCP_EXT_dontgo_e;
	}while(0);
	return 0;
}
void init_server_secret(NET_TCP_t *tcp, uint64_t secret){
	IO_print(FD_OUT, "server secret is 0x%llx\n", secret);
	NET_TCP_eid_t eid = NET_TCP_EXT_new(tcp, sizeof(secret), 1);
	uint64_t *ssecret = (uint64_t *)NET_TCP_EXT_get_sockdata(tcp, eid);
	*ssecret = secret;
	NET_TCP_EXTcbadd(tcp, NET_TCP_oid_connstate_e, eid, (void *)server_secret_connstate_cb);
	NET_TCP_EXTcbadd(tcp, NET_TCP_oid_read_e, eid, (void *)server_secret_read_cb);
}
uint32_t client_secret_connstate_cb(NET_TCP_peer_t *peer, void *sd, uint64_t *psecret, uint8_t flag){
	if(!(flag & NET_TCP_connstate_succ_e)){
		return 0;
	}
	NET_TCP_qsend_ptr(peer, psecret, sizeof(*psecret));
	return 0;
}
NET_TCP_eid_t init_client_secret(NET_TCP_t *tcp){
	NET_TCP_eid_t eid = NET_TCP_EXT_new(tcp, 0, sizeof(uint64_t));
	NET_TCP_EXTcbadd(tcp, NET_TCP_oid_connstate_e, eid, (void *)client_secret_connstate_cb);
	return eid;
}
void init_client_secret_peerdata(NET_TCP_peer_t *peer, NET_TCP_eid_t eid, uint64_t secret){
	uint64_t *psecret = (uint64_t *)NET_TCP_EXT_get_peerdata(peer, eid);
	*psecret = secret;
}

enum{
	PACKET_FRAME,
	PACKET_KEYFRAME,
	PACKET_CURSOR,
	PACKET_KEY,
	PACKET_TOTAL
};

#pragma pack(push, 1)
typedef struct{
	uint16_t x;
	uint16_t y;
}packet_cursor_t;
typedef struct{
	uint16_t key;
	uint8_t action;
}packet_key_t;
#pragma pack(pop)

void send_packet_frame(NET_TCP_peer_t *peer, uint32_t size){
	#pragma pack(push, 1)
	struct{
		uint8_t type;
		uint32_t size;
	}data;
	#pragma pack(pop)
	data.type = PACKET_FRAME;
	data.size = size;
	NET_TCP_qsend_ptr(peer, &data, sizeof(data));
}

void send_packet_keyframe(NET_TCP_peer_t *peer, uint32_t size){
	#pragma pack(push, 1)
	struct{
		uint8_t type;
		uint32_t size;
	}data;
	#pragma pack(pop)
	data.type = PACKET_KEYFRAME;
	data.size = size;
	NET_TCP_qsend_ptr(peer, &data, sizeof(data));
}

void send_packet_cursor(NET_TCP_peer_t *peer, uint16_t x, uint16_t y){
	#pragma pack(push, 1)
	struct{
		uint8_t type;
		packet_cursor_t c;
	}data;
	#pragma pack(pop)
	data.type = PACKET_CURSOR;
	data.c.x = x;
	data.c.y = y;
	NET_TCP_qsend_ptr(peer, &data, sizeof(data));
}

void send_packet_key(NET_TCP_peer_t *peer, uint16_t key, uint8_t action){
	#pragma pack(push, 1)
	struct{
		uint8_t type;
		packet_key_t k;
	}data;
	#pragma pack(pop)
	data.type = PACKET_KEY;
	data.k.key = key;
	data.k.action = action;
	NET_TCP_qsend_ptr(peer, &data, sizeof(data));
}

typedef struct{
	uint8_t type;
	union{
		struct{
			uint8_t round;
			uint32_t size;
		}frame;
	}s;
}ptype_t;

typedef bool (*packet_cb_t)(void *u0, void *u1, void *u2);
bool process_incoming_packet(
	void *u0,
	void *u1,
	void *u2,
	uint8_t *data,
	uint_t size,
	ptype_t *ptype,
	VEC_t *packet,
	packet_cb_t frame_cb,
	packet_cb_t keyframe_cb,
	packet_cb_t cursor_cb,
	packet_cb_t key_cb
){
begin_gt:
	switch(ptype->type){
		case PACKET_FRAME:{
			switch(ptype->s.frame.round){
				case 0:{
					if((packet->Current + size) >= sizeof(uint32_t)){
						uint8_t pushed = sizeof(uint32_t) - packet->Current;
						VEC_pushbackn(packet, data, pushed);
						ptype->s.frame.round = 1;
						ptype->s.frame.size = *(uint32_t *)packet->ptr;
						packet->Current = 0;
						data += pushed;
						size -= pushed;
					}
					else{
						VEC_pushbackn(packet, data, size);
						return 0;
					}
				}
				case 1:{
					if((packet->Current + size) >= ptype->s.frame.size){
						uint32_t pushed = ptype->s.frame.size - packet->Current;
						VEC_pushbackn(packet, data, pushed);
						frame_cb(u0, u1, u2);
						data += pushed;
						size -= pushed;
						packet->Current = 0;
						ptype->type = PACKET_TOTAL;
						goto begin_gt;
					}
					else{
						VEC_pushbackn(packet, data, size);
						return 0;
					}
				}
			}
			break;
		}
		case PACKET_KEYFRAME:{
			switch(ptype->s.frame.round){
				case 0:{
					if((packet->Current + size) >= sizeof(uint32_t)){
						uint8_t pushed = sizeof(uint32_t) - packet->Current;
						VEC_pushbackn(packet, data, pushed);
						ptype->s.frame.round = 1;
						ptype->s.frame.size = *(uint32_t *)packet->ptr;
						packet->Current = 0;
						data += pushed;
						size -= pushed;
					}
					else{
						VEC_pushbackn(packet, data, size);
						return 0;
					}
				}
				case 1:{
					if((packet->Current + size) >= ptype->s.frame.size){
						uint32_t pushed = ptype->s.frame.size - packet->Current;
						VEC_pushbackn(packet, data, pushed);
						keyframe_cb(u0, u1, u2);
						data += pushed;
						size -= pushed;
						packet->Current = 0;
						ptype->type = PACKET_TOTAL;
						goto begin_gt;
					}
					else{
						VEC_pushbackn(packet, data, size);
						return 0;
					}
				}
			}
			break;
		}
		case PACKET_CURSOR:{
			if((packet->Current + size) >= sizeof(packet_cursor_t)){
				uint_t pushed = sizeof(packet_cursor_t) - packet->Current;
				VEC_pushbackn(packet, data, pushed);
				cursor_cb(u0, u1, u2);
				data += pushed;
				size -= pushed;
				packet->Current = 0;
				ptype->type = PACKET_TOTAL;
				goto begin_gt;
			}
			else{
				VEC_pushbackn(packet, data, size);
				return 0;
			}
			break;
		}
		case PACKET_KEY:{
			if((packet->Current + size) >= sizeof(packet_key_t)){
				uint8_t pushed = sizeof(packet_key_t) - packet->Current;
				VEC_pushbackn(packet, data, pushed);
				key_cb(u0, u1, u2);
				data += pushed;
				size -= pushed;
				packet->Current = 0;
				ptype->type = PACKET_TOTAL;
				goto begin_gt;
			}
			else{
				VEC_pushbackn(packet, data, size);
				return 0;
			}
			break;
		}
		case PACKET_TOTAL:{
			if(!size){
				return 0;
			}
			ptype->type = data[0];
			switch(ptype->type){
				case PACKET_FRAME:{
					ptype->s.frame.round = 0;
					break;
				}
				case PACKET_KEYFRAME:{
					ptype->s.frame.round = 0;
					break;
				}
				case PACKET_CURSOR:{
					break;
				}
				case PACKET_KEY:{
					break;
				}
				default:{
					IO_print(FD_OUT, "weird byte: %lu\n", ptype->type);
					assert(0);
				}
			}
			data++;
			size--;
			goto begin_gt;
		}
	}
	return 0;
}

typedef struct{
	VAS_t peers;
	IO_SCR_t scr;
	struct{
		AV_codec_t *codec;
		AV_dict_t *dict;
		AV_context_t *context;
		AV_frame_t *frame;
		AV_packet_t *packet;
		VEC_t initialdata;
		uint64_t last;
		uint32_t fps;
	}av;
	EV_ev_t_t evt;
}com_grab_sockdata_t;
typedef struct{
	VAS_node_t node;
	ptype_t ptype;
	VEC_t packet;
}com_grab_peerdata_t;
void com_grab_encode_cb(EV_t *listener, EV_ev_t_t *evt, uint32_t flag){
	uint64_t t0 = T_nowi();
	com_grab_sockdata_t *sd = OFFSETLESS(evt, com_grab_sockdata_t, evt);
	uint8_t *pixelbuf = IO_SCR_read(&sd->scr);
	assert(pixelbuf);
	assert(!AV_frame_write(listener, sd->av.frame, pixelbuf, sd->scr.res.x, sd->scr.res.y, AV_PIX_FMT_BGRA));
	assert(AV_inwrite(sd->av.context, sd->av.frame) > 0);
	IO_ssize_t rinread;
	while((rinread = AV_inread(sd->av.context, sd->av.packet)) > 0){
		if(sd->av.packet->flags & AV_PKT_FLAG_KEY){
			sd->av.initialdata.Current = 0;
			VEC_pushbackn(&sd->av.initialdata, sd->av.packet->data, rinread);
		}
		VAS_node_t inode = *VAS_road0(&sd->peers, sd->peers.src);
		while(inode != sd->peers.dst){
			NET_TCP_peer_t *peer = *(NET_TCP_peer_t **)VAS_out(&sd->peers, inode);
			send_packet_frame(peer, rinread);
			NET_TCP_qsend_ptr(peer, sd->av.packet->data, rinread);
			inode = *VAS_road0(&sd->peers, inode);
		}
	}
	assert(rinread != -1);
	sd->av.fps++;
	uint64_t t1 = T_nowi();
	uint64_t result = t1 - t0;
	uint64_t expected = (uint64_t)1000000000 / sd->av.context->time_base.den;
	if(t1 > (sd->av.last + 1000000000)){
		sd->av.last = t1;
		if(sd->av.context->time_base.den > sd->av.fps){
			IO_print(FD_OUT, "OVERLOAD fps result %lu expected %lu\n", sd->av.fps, sd->av.context->time_base.den);
		}
		sd->av.fps = 0;
	}
	if(result >= expected){
		//IO_print(FD_OUT, "OVERLOAD encode result %llu expected %llu\n", result, expected);
	}
}
uint32_t com_grab_connstate_cb(NET_TCP_peer_t *peer, com_grab_sockdata_t *sd, com_grab_peerdata_t *pd, uint8_t flag){
	if(flag & NET_TCP_connstate_succ_e){
		IO_print(FD_OUT, "[+] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
		pd->ptype.type = PACKET_TOTAL;
		VEC_init(&pd->packet, 1);
		pd->node = VAS_getnode_dst(&sd->peers);
		*(NET_TCP_peer_t **)VAS_out(&sd->peers, pd->node) = peer;
		send_packet_frame(peer, sd->av.initialdata.Current);
		NET_TCP_qsend_ptr(peer, sd->av.initialdata.ptr, sd->av.initialdata.Current);
	}
	else do{
		IO_print(FD_OUT, "[-] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
		if(!(flag & NET_TCP_connstate_init_e)){
			break;
		}
		VAS_unlink(&sd->peers, pd->node);
		VEC_free(&pd->packet);
	}while(0);

	return 0;
}
void com_grab_cursor_cb(NET_TCP_peer_t *peer, com_grab_sockdata_t *sd, com_grab_peerdata_t *pd){
	packet_cursor_t *cursor = (packet_cursor_t *)pd->packet.ptr;
}
void com_grab_key_cb(NET_TCP_peer_t *peer, com_grab_sockdata_t *sd, com_grab_peerdata_t *pd){
	packet_key_t *key = (packet_key_t *)pd->packet.ptr;
}
uint32_t com_grab_read_cb(NET_TCP_peer_t *peer, com_grab_sockdata_t *sd, com_grab_peerdata_t *pd, uint8_t **data, uint_t *size){
	bool r = process_incoming_packet(
		peer,
		sd,
		pd,
		*data,
		*size,
		&pd->ptype,
		&pd->packet,
		(packet_cb_t)EMPTY_FUNCTION,
		(packet_cb_t)EMPTY_FUNCTION,
		(packet_cb_t)com_grab_cursor_cb,
		(packet_cb_t)com_grab_key_cb
	);
	assert(!r);
	return 0;
}

struct com_view_peerdata_t{
	ptype_t ptype;
	VEC_t packet;
	VEC_t pixmap;

	struct{
		AV_codec_t *codec;
		AV_context_t *context;
		AV_frame_t *frame;
		AV_packet_t *packet;
	}av;

	EV_ev_t_t tmain;

	fan::window* window;
	fan::camera* camera;
	fan_2d::sprite* image;
};
void com_view_main_cb(EV_t* listener, EV_ev_t_t* evt, uint32_t flag){
	com_view_peerdata_t *pd = OFFSETLESS(evt, com_view_peerdata_t, tmain);

	pd->window->execute(0, [&]{
		pd->window->get_fps();
		pd->image->draw();
	});
	//fan::window::handle_events();
}
uint32_t com_view_connstate_cb(NET_TCP_peer_t* peer, void *sd, com_view_peerdata_t* pd, uint8_t flag){
	if(flag & NET_TCP_connstate_succ_e){
		pd->ptype.type = PACKET_TOTAL;
		VEC_init(&pd->packet, 1, AV_resize);
		VEC_init(&pd->pixmap, 1, AV_resize);

		pd->av.codec = AV_decoder_open(AV_CODEC_ID_H264);
		assert(pd->av.codec);
		pd->av.context = AV_context_open(pd->av.codec, 0);
		assert(pd->av.context);
		assert(!AV_context_set(pd->av.codec, pd->av.context, 0));
		pd->av.frame = AV_frame_alloc();
		assert(pd->av.frame);
		pd->av.packet = AV_packet_open();
		assert(pd->av.packet);

		EV_ev_t_init(&pd->tmain, .001, (EV_ev_cb_t)com_view_main_cb);
		EV_ev_t_start(peer->parent->listener, &pd->tmain);

		pd->window = new fan::window();
		pd->window->set_vsync(true);
		pd->window->auto_close(false);
		pd->camera = new fan::camera(pd->window);

		pd->window->add_mouse_move_callback([peer](fan::window* window){
			fan::vec2 position = window->get_mouse_position();
			send_packet_cursor(peer, position.x, position.y);
		});

		pd->window->add_close_callback([peer]{
			NET_TCP_closehard(peer);
		});

		pd->window->add_key_callback(fan::key_escape, [&]{
			NET_TCP_closehard(peer);
		});

		fan_2d::image_load_properties::internal_format = GL_RGB;
		fan_2d::image_load_properties::format = GL_RGB;
		fan_2d::image_load_properties::type = GL_UNSIGNED_BYTE;

		pd->image = new fan_2d::sprite(pd->camera);

		pd->window->set_error_callback();

		IO_print(FD_OUT, "[+] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
	}
	else do{
		IO_print(FD_OUT, "[-] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
		if(!(flag & NET_TCP_connstate_init_e)){
			break;
		}

		VEC_free(&pd->pixmap);

		AV_codec_close(pd->av.codec);
		AV_context_close(pd->av.context);
		AV_frame_close(pd->av.frame);
		AV_packet_close(pd->av.packet);

		EV_ev_t_stop(peer->parent->listener, &pd->tmain);

		delete pd->window;
		delete pd->camera;
		delete pd->image;
	}while(0);
	return 0;
}
void com_view_frame_cb(NET_TCP_peer_t *peer, void *sd, com_view_peerdata_t *pd){
	IO_ssize_t routwrite = AV_outwrite(pd->av.context, pd->packet.ptr, pd->packet.Current, pd->av.packet);
	assert(routwrite == pd->packet.Current);
	IO_ssize_t routread = AV_outread(pd->av.context, pd->av.frame);
	assert(routread >= 0);
	if(!routread){
		return;
	}
	pd->pixmap.Current = 0;
	VEC_handle0(&pd->pixmap, pd->av.frame->width * pd->av.frame->height * 3);
	assert(!AV_frame_read(pd->av.frame, pd->pixmap.ptr, pd->av.frame->width, pd->av.frame->height, AV_PIX_FMT_RGB24));
	pd->image->reload_sprite(0, pd->pixmap.ptr, fan::vec2i(pd->av.frame->width, pd->av.frame->height));
	pd->image->set_size(0, pd->window->get_size());
}
void com_view_cursor_cb(NET_TCP_peer_t *peer, void *sd, com_view_peerdata_t *pd){
	packet_cursor_t *cursor = (packet_cursor_t *)pd->packet.ptr;
}
void com_view_key_cb(NET_TCP_peer_t *peer, void *sd, com_view_peerdata_t *pd){
	packet_key_t *key = (packet_key_t *)pd->packet.ptr;
}
uint32_t com_view_read_cb(NET_TCP_peer_t *peer, void *sd, com_view_peerdata_t *pd, uint8_t **data, uint_t *size){
	bool r = process_incoming_packet(
		peer,
		sd,
		pd,
		*data,
		*size,
		&pd->ptype,
		&pd->packet,
		(packet_cb_t)com_view_frame_cb,
		(packet_cb_t)com_view_frame_cb,
		(packet_cb_t)com_view_cursor_cb,
		(packet_cb_t)com_view_key_cb
	);
	assert(!r);
	return 0;
}
bool com_view_init(base_t* base){
	base->net.tcp.tcpview = NET_TCP_alloc(&base->listener);

	init_tls(base->net.tcp.tcpview);
	base->net.tcp.tcpview_secret_eid = init_client_secret(base->net.tcp.tcpview);
	NET_TCP_eid_t eid = NET_TCP_EXT_new(base->net.tcp.tcpview, 0, sizeof(com_view_peerdata_t));
	NET_TCP_EXTcbadd(base->net.tcp.tcpview, NET_TCP_oid_connstate_e, eid, (void *)com_view_connstate_cb);
	NET_TCP_EXTcbadd(base->net.tcp.tcpview, NET_TCP_oid_read_e, eid, (void *)com_view_read_cb);

	return 0;
}

typedef struct{
	VAS_t peers;
	NET_TCP_eid_t eid;
	NET_TCP_peer_t *main_peer;
	struct{
		VEC_t initialdata;
	}av;
	ptype_t ptype;
	VEC_t packet;
}com_grabfrom_sockdata_t;
typedef struct{
	VAS_node_t node;
	bool state;
	ptype_t ptype;
	VEC_t packet;
}com_grabfrom_peerdata_t;
uint32_t com_grabfrom_connstate_cb(NET_TCP_peer_t *peer, com_grabfrom_sockdata_t *sd, com_grabfrom_peerdata_t *pd, uint8_t flag){
	if(flag & NET_TCP_connstate_succ_e){
		pd->ptype.type = PACKET_TOTAL;
		VEC_init(&pd->packet, 1);
		if(!sd->main_peer){
			IO_print(FD_OUT, "[+] main peer %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
			sd->main_peer = peer;
		}
		else{
			IO_print(FD_OUT, "[+] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
			pd->node = VAS_getnode_dst(&sd->peers);
			pd->state = 0;
			*(NET_TCP_peer_t **)VAS_out(&sd->peers, pd->node) = peer;
			if(sd->av.initialdata.Current){
				send_packet_keyframe(peer, sd->av.initialdata.Current);
				NET_TCP_qsend_ptr(peer, sd->av.initialdata.ptr, sd->av.initialdata.Current);
			}
		}
	}
	else do{
		if(!(flag & NET_TCP_connstate_init_e)){
			break;
		}
		if(peer == sd->main_peer){
			IO_print(FD_OUT, "[-] main peer %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
			sd->main_peer = 0;
		}
		else{
			IO_print(FD_OUT, "[-] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
			VAS_unlink(&sd->peers, pd->node);
		}
	}while(0);

	return 0;
}
void com_grabfrom_keyframe_cb(NET_TCP_peer_t *peer, com_grabfrom_sockdata_t *sd, void *pd){
	if(peer != sd->main_peer){
		return;
	}
	sd->av.initialdata.Current = 0;
	VEC_pushbackn(&sd->av.initialdata, sd->packet.ptr, sd->packet.Current);
}
void com_grabfrom_cursor_cb(NET_TCP_peer_t *peer, com_grabfrom_sockdata_t *sd, void *pd){
	if(peer == sd->main_peer){
		return;
	}
	/* TODO send it to main peer */
}
void com_grabfrom_key_cb(NET_TCP_peer_t *peer, com_grabfrom_sockdata_t *sd, void *pd){
	if(peer == sd->main_peer){
		return;
	}
	/* TODO send it to main peer */
}
uint32_t com_grabfrom_read_cb(NET_TCP_peer_t *peer, com_grabfrom_sockdata_t *sd, com_grabfrom_peerdata_t *pd, uint8_t **data, uint_t *size){
	if(peer == sd->main_peer){
		VAS_node_t inode = *VAS_road0(&sd->peers, sd->peers.src);
		while(inode != sd->peers.dst){
			NET_TCP_peer_t *npeer = *(NET_TCP_peer_t **)VAS_out(&sd->peers, inode);
			com_grabfrom_peerdata_t *npd = (com_grabfrom_peerdata_t *)NET_TCP_EXT_get_peerdata(npeer, sd->eid);
			if(npd->state){
				NET_TCP_qsend_ptr(npeer, *data, *size);
			}
			else do{
				/* this means this packet is first of rest */
				/* because ptype still didnt assigned */
				if(sd->ptype.type != PACKET_TOTAL){
					break;
				}
				npd->state = 1;
				NET_TCP_qsend_ptr(npeer, *data, *size);
			}while(0);
			inode = *VAS_road0(&sd->peers, inode);
		}
		bool r = process_incoming_packet(
			peer,
			sd,
			0,
			*data,
			*size,
			&sd->ptype,
			&sd->packet,
			(packet_cb_t)EMPTY_FUNCTION,
			(packet_cb_t)com_grabfrom_keyframe_cb,
			(packet_cb_t)com_grabfrom_cursor_cb,
			(packet_cb_t)com_grabfrom_key_cb
		);
		assert(!r);
	}
	else{
		bool r = process_incoming_packet(
			peer,
			sd,
			pd,
			*data,
			*size,
			&pd->ptype,
			&pd->packet,
			(packet_cb_t)EMPTY_FUNCTION,
			(packet_cb_t)com_grabfrom_keyframe_cb,
			(packet_cb_t)com_grabfrom_cursor_cb,
			(packet_cb_t)com_grabfrom_key_cb
		);
		assert(!r);
	}
	return 0;
}

typedef struct{
	NET_TCP_peer_t *peer;

	IO_SCR_t scr;
	struct{
		AV_codec_t *codec;
		AV_dict_t *dict;
		AV_context_t *context;
		AV_frame_t *frame;
		AV_packet_t *packet;
		uint64_t last;
		uint32_t fps;
	}av;
	EV_ev_t_t evt;

	ptype_t ptype;
	VEC_t packet;
}com_grabto_peerdata_t;
void com_grabto_encode_cb(EV_t *listener, EV_ev_t_t *evt, uint32_t flag){
	uint64_t t0 = T_nowi();
	com_grabto_peerdata_t *pd = OFFSETLESS(evt, com_grabto_peerdata_t, evt);
	uint8_t *pixelbuf = IO_SCR_read(&pd->scr);
	assert(pixelbuf);
	assert(!AV_frame_write(listener, pd->av.frame, pixelbuf, pd->scr.res.x, pd->scr.res.y, AV_PIX_FMT_BGRA));
	assert(AV_inwrite(pd->av.context, pd->av.frame) > 0);
	IO_ssize_t rinread;
	while((rinread = AV_inread(pd->av.context, pd->av.packet)) > 0){
		if(pd->av.packet->flags & AV_PKT_FLAG_KEY){
			send_packet_keyframe(pd->peer, rinread);
			NET_TCP_qsend_ptr(pd->peer, pd->av.packet->data, rinread);
		}
		else{
			send_packet_frame(pd->peer, rinread);
			NET_TCP_qsend_ptr(pd->peer, pd->av.packet->data, rinread);
		}
	}
	assert(rinread != -1);
	pd->av.fps++;
	uint64_t t1 = T_nowi();
	uint64_t result = t1 - t0;
	uint64_t expected = (uint64_t)1000000000 / pd->av.context->time_base.den;
	if(t1 > (pd->av.last + 1000000000)){
		pd->av.last = t1;
		if(pd->av.context->time_base.den > pd->av.fps){
			IO_print(FD_OUT, "OVERLOAD fps result %lu expected %lu\n", pd->av.fps, pd->av.context->time_base.den);
		}
		pd->av.fps = 0;
	}
	if(result >= expected){
		IO_print(FD_OUT, "OVERLOAD encode result %llu expected %llu\n", result, expected);
	}
}
uint32_t com_grabto_connstate_cb(NET_TCP_peer_t *peer, void *sd, com_grabto_peerdata_t *pd, uint8_t flag){
	if(flag & NET_TCP_connstate_succ_e){
		IO_print(FD_OUT, "[+] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
		EV_ev_t_start(peer->parent->listener, &pd->evt);
	}
	else do{
		IO_print(FD_OUT, "[-] %08x%04x\n", peer->sdstaddr.ip, peer->sdstaddr.port);
		/* this place needs alot free */
		if(!(flag & NET_TCP_connstate_init_e)){
			break;
		}
		EV_ev_t_stop(peer->parent->listener, &pd->evt);
	}while(0);

	return 0;
}
void com_grabto_cursor_cb(NET_TCP_peer_t *peer, void *sd, com_grabto_peerdata_t *pd){
	packet_cursor_t *cursor = (packet_cursor_t *)pd->packet.ptr;
}
void com_grabto_key_cb(NET_TCP_peer_t *peer, void *sd, com_grabto_peerdata_t *pd){
	packet_key_t *key = (packet_key_t *)pd->packet.ptr;
}
uint32_t com_grabto_read_cb(NET_TCP_peer_t *peer, void *sd, com_grabto_peerdata_t *pd, uint8_t **data, uint_t *size){
	bool r = process_incoming_packet(
		peer,
		sd,
		pd,
		*data,
		*size,
		&pd->ptype,
		&pd->packet,
		(packet_cb_t)EMPTY_FUNCTION,
		(packet_cb_t)EMPTY_FUNCTION,
		(packet_cb_t)com_grabto_cursor_cb,
		(packet_cb_t)com_grabto_key_cb
	);
	assert(!r);
	return 0;
}
bool com_grabto_init(base_t *base){
	base->net.tcp.tcpgrabto = NET_TCP_alloc(&base->listener);

	init_tls(base->net.tcp.tcpgrabto);
	base->net.tcp.tcpgrabto_secret_eid = init_client_secret(base->net.tcp.tcpgrabto);
	base->net.tcp.tcpgrabto_eid = NET_TCP_EXT_new(base->net.tcp.tcpgrabto, 0, sizeof(com_grabto_peerdata_t));
	NET_TCP_EXTcbadd(base->net.tcp.tcpgrabto, NET_TCP_oid_connstate_e, base->net.tcp.tcpgrabto_eid, (void *)com_grabto_connstate_cb);
	NET_TCP_EXTcbadd(base->net.tcp.tcpgrabto, NET_TCP_oid_read_e, base->net.tcp.tcpgrabto_eid, (void *)com_grabto_read_cb);

	return 0;
}

VAS_node_t com_grab(base_t *base, uint16_t port, uint64_t secret, uint32_t framerate, uint32_t rate){
	VAS_node_t node = VAS_getnode_dst(&base->net.tcp.server);
	NET_TCP_t **tcp = (NET_TCP_t **)VAS_out(&base->net.tcp.server, node);

	*tcp = NET_TCP_alloc(&base->listener);
	(*tcp)->ssrcaddr.port = port;

	init_tls(*tcp);
	init_server_secret(*tcp, secret);
	NET_TCP_eid_t eid = NET_TCP_EXT_new(*tcp, sizeof(com_grab_sockdata_t), sizeof(com_grab_peerdata_t));
	com_grab_sockdata_t *sd = (com_grab_sockdata_t *)NET_TCP_EXT_get_sockdata(*tcp, eid);

	VAS_open(&sd->peers, sizeof(NET_TCP_peer_t *));

	NET_TCP_EXTcbadd(*tcp, NET_TCP_oid_connstate_e, eid, (void *)com_grab_connstate_cb);
	NET_TCP_EXTcbadd(*tcp, NET_TCP_oid_read_e, eid, (void *)com_grab_read_cb);

	if(NET_TCP_listen(*tcp)){
		NET_TCP_free(*tcp);
		VAS_unlink(&base->net.tcp.server, node);
		return (VAS_node_t)-1;
	}
	EV_ev_io_start(&base->listener, &(*tcp)->ev);

	assert(!IO_SCR_open(&sd->scr));

	sd->av.codec = AV_encoder_open(AV_CODEC_ID_H264);
	assert(sd->av.codec);
	sd->av.dict = 0;
	assert(!AV_dict_set(&sd->av.dict, "preset", "veryfast", 0));
	assert(!AV_dict_set(&sd->av.dict, "tune", "zerolatency", 0));
	sd->av.context = AV_context_open(sd->av.codec, framerate);
	assert(sd->av.context);
	sd->av.context->width = sd->scr.res.x;
	sd->av.context->height = sd->scr.res.y;
	AV_context_cbr(sd->av.context, rate);
	assert(!AV_context_set(sd->av.codec, sd->av.context, &sd->av.dict));
	sd->av.frame = AV_frame_open(sd->av.context);
	assert(sd->av.frame);
	sd->av.packet = AV_packet_open();
	assert(sd->av.packet);
	VEC_init(&sd->av.initialdata, 1);
	sd->av.last = T_nowi();
	sd->av.fps = 0;

	uint8_t *pixelbuf = A_resize(0, sd->scr.res.x * sd->scr.res.y * 3);
	MEM_set(0, pixelbuf, sd->scr.res.x * sd->scr.res.y * 3);
	assert(!AV_frame_write(&base->listener, sd->av.frame, pixelbuf, sd->scr.res.x, sd->scr.res.y, AV_PIX_FMT_RGB24));
	A_resize(pixelbuf, 0);
	assert(AV_inwrite(sd->av.context, sd->av.frame) > 0);
	IO_ssize_t rinread;
	while((rinread = AV_inread(sd->av.context, sd->av.packet)) > 0){
		VEC_pushbackn(&sd->av.initialdata, sd->av.packet->data, rinread);
	}
	assert(rinread >= 0);

	EV_ev_t_init(&sd->evt, (f64_t)1 / framerate, (EV_ev_cb_t)com_grab_encode_cb);
	EV_ev_t_start(&base->listener, &sd->evt);

	return node;
}

NET_TCP_peer_t *com_view(base_t *base, NET_addr_t addr, uint64_t secret){
	NET_TCP_peer_t *peer = NET_TCP_connect(base->net.tcp.tcpview, addr);
	if(!peer){
		return 0;
	}
	init_client_secret_peerdata(peer, base->net.tcp.tcpview_secret_eid, secret);
	EV_ev_tio_start(&base->listener, &peer->ev);
	return peer;
}

VAS_node_t com_grabfrom(base_t *base, uint16_t port, uint64_t secret){
	VAS_node_t node = VAS_getnode_dst(&base->net.tcp.server);
	NET_TCP_t **tcp = (NET_TCP_t **)VAS_out(&base->net.tcp.server, node);

	*tcp = NET_TCP_alloc(&base->listener);
	(*tcp)->ssrcaddr.port = port;

	init_tls(*tcp);
	init_server_secret(*tcp, secret);
	NET_TCP_eid_t eid = NET_TCP_EXT_new(*tcp, sizeof(com_grabfrom_sockdata_t), sizeof(com_grabfrom_peerdata_t));
	com_grabfrom_sockdata_t *sd = (com_grabfrom_sockdata_t *)NET_TCP_EXT_get_sockdata(*tcp, eid);
	sd->eid = eid;

	VAS_open(&sd->peers, sizeof(NET_TCP_peer_t *));

	sd->main_peer = 0;

	VEC_init(&sd->av.initialdata, 1);

	sd->ptype.type = PACKET_TOTAL;
	VEC_init(&sd->packet, 1);

	NET_TCP_EXTcbadd(*tcp, NET_TCP_oid_connstate_e, eid, (void *)com_grabfrom_connstate_cb);
	NET_TCP_EXTcbadd(*tcp, NET_TCP_oid_read_e, eid, (void *)com_grabfrom_read_cb);

	if(NET_TCP_listen(*tcp)){
		NET_TCP_free(*tcp);
		VAS_unlink(&base->net.tcp.server, node);
		return (VAS_node_t)-1;
	}
	EV_ev_io_start(&base->listener, &(*tcp)->ev);

	return node;
}

bool com_grabto(base_t *base, NET_addr_t addr, uint64_t secret, uint32_t framerate, uint32_t rate){
	NET_TCP_peer_t *peer = NET_TCP_connect(base->net.tcp.tcpgrabto, addr);

	init_client_secret_peerdata(peer, base->net.tcp.tcpgrabto_secret_eid, secret);

	com_grabto_peerdata_t *pd = (com_grabto_peerdata_t *)NET_TCP_EXT_get_peerdata(peer, base->net.tcp.tcpgrabto_eid);

	pd->peer = peer;

	assert(!IO_SCR_open(&pd->scr));

	pd->av.codec = AV_encoder_open(AV_CODEC_ID_H264);
	assert(pd->av.codec);
	pd->av.dict = 0;
	assert(!AV_dict_set(&pd->av.dict, "preset", "veryfast", 0));
	assert(!AV_dict_set(&pd->av.dict, "tune", "zerolatency", 0));
	pd->av.context = AV_context_open(pd->av.codec, framerate);
	assert(pd->av.context);
	pd->av.context->width = pd->scr.res.x;
	pd->av.context->height = pd->scr.res.y;
	AV_context_cbr(pd->av.context, rate);
	assert(!AV_context_set(pd->av.codec, pd->av.context, &pd->av.dict));
	pd->av.frame = AV_frame_open(pd->av.context);
	assert(pd->av.frame);
	pd->av.packet = AV_packet_open();
	assert(pd->av.packet);
	pd->av.last = T_nowi();
	pd->av.fps = 0;

	pd->ptype.type = PACKET_TOTAL;
	VEC_init(&pd->packet, 1);

	EV_ev_t_init(&pd->evt, (f64_t)1 / framerate, (EV_ev_cb_t)com_grabto_encode_cb);

	return 0;
}

void gui_main_cb(EV_t *listener, EV_ev_t_t *evt, uint32_t flag){
	base_t *base = OFFSETLESS(evt, base_t, gui.evt);
	base->gui.window.execute(0, [&]{
		if (base->gui.window.key_press(fan::mouse_left)) {
			bool found = false;

			for (int i = 0; i < base->gui.stb.size(); i++) {
				base->gui.stb.get_mouse_cursor(i);
			}

			if (!found) {
				//fan_2d::gui::current_focus[base->gui.window.get_handle()] = -1;
			}
		}

		base->gui.stb.draw();
		base->gui.boxes.draw();
		base->gui.tr.draw();
	});

	fan::window::handle_events();
}

void run(base_t* base){
	EV_open(&base->listener);

	VAS_open(&base->net.tcp.server, sizeof(NET_TCP_t *));
	com_view_init(base);
	com_grabto_init(base);

	base->gui.window.set_vsync(false);
	base->gui.window.add_close_callback([&]{
		PR_exit(0);
	});
	base->gui.window.add_key_callback(fan::key_escape, [&]{
		PR_exit(0);
	});

	base->gui.boxes.push_back(L"grab", font_size, base->gui.button(5), base->gui.box_size, base->gui.border_size , fan::colors::purple - 0.4);
	base->gui.boxes.push_back(L"view", font_size, base->gui.button(4), base->gui.box_size, base->gui.border_size, fan::colors::purple - 0.4);
	base->gui.boxes.push_back(L"grab from", font_size, base->gui.button(3), base->gui.box_size, base->gui.border_size , fan::colors::purple - 0.4);
	base->gui.boxes.push_back(L"grab to", font_size, base->gui.button(2), base->gui.box_size, base->gui.border_size , fan::colors::purple - 0.4);
	base->gui.boxes.push_back(L"start", font_size, base->gui.button(0), base->gui.box_size, base->gui.border_size, fan::colors::purple - 0.4);

	base->gui.window.add_resize_callback([&] {
		for (int i = 0; i < base->gui.tr.size(); i++) {
			const auto offset = fan_2d::gui::get_resize_movement_offset(base->gui.camera.m_window);
			base->gui.tr.set_position(i, base->gui.tr.get_position(i) + offset);
		}
	});

	base->gui.boxes.on_click([&](uint_t i) {
		auto selected = base->gui.boxes.get_selected();

		if (selected != fan::uninitialized) {
			base->gui.boxes.set_box_color(selected, fan::colors::purple - 0.4);
		}

		switch (i) {
			case 0:{
				base->gui.formBegin();
				base->gui.formPush(L"Port:", fan::colors::white, L"8081", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Secret:", fan::colors::white, L"123", fan::colors::cyan - 0.9);
				base->gui.formPush(L"FPS:", fan::colors::white, L"20", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Rate:", fan::colors::white, L"200000", fan::colors::cyan - 0.9);
				base->gui.formEnd(0);

				base->gui.boxes.set_box_color(i, fan::colors::purple - 0.3);
				base->gui.boxes.set_selected(i);

				break;
			}
			case 1:{
				base->gui.formBegin();
				base->gui.formPush(L"IP:", fan::colors::white, L"127.0.0.1", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Port:", fan::colors::white, L"8081", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Secret:", fan::colors::white, L"123", fan::colors::cyan - 0.9);
				base->gui.formEnd(0);

				base->gui.boxes.set_box_color(i, fan::colors::purple - 0.3);
				base->gui.boxes.set_selected(i);

				break;
			}
			case 2:{
				base->gui.formBegin();
				base->gui.formPush(L"Port:", fan::colors::white, L"8081", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Secret:", fan::colors::white, L"123", fan::colors::cyan - 0.9);
				base->gui.formEnd(0);

				base->gui.boxes.set_box_color(i, fan::colors::purple - 0.3);
				base->gui.boxes.set_selected(i);

				break;
			}
			case 3:{
				base->gui.formBegin();
				base->gui.formPush(L"IP:", fan::colors::white, L"127.0.0.1", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Port:", fan::colors::white, L"8081", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Secret:", fan::colors::white, L"123", fan::colors::cyan - 0.9);
				base->gui.formPush(L"FPS:", fan::colors::white, L"20", fan::colors::cyan - 0.9);
				base->gui.formPush(L"Rate:", fan::colors::white, L"200000", fan::colors::cyan - 0.9);
				base->gui.formEnd(0);

				base->gui.boxes.set_box_color(i, fan::colors::purple - 0.3);
				base->gui.boxes.set_selected(i);

				break;
			}
			case 4:{
				auto selected = base->gui.boxes.get_selected();
				switch(selected){
					case 0:{
						uint16_t port = std::stoi(base->gui.stb.get_line(0, 0));
						uint64_t secret = std::stoi(base->gui.stb.get_line(1, 0));
						uint32_t fps = std::stoi(base->gui.stb.get_line(2, 0));
						uint32_t rate = std::stoi(base->gui.stb.get_line(3, 0));

						VAS_node_t node = com_grab(base, port, secret, fps, rate);
						assert(node != (VAS_node_t)-1);

						break;
					}
					case 1:{
						uint8_t sip[4];
						uint_t pi = 0;

						auto wstr = base->gui.stb.get_line(0, 0);

						if (STR_rscancc(std::string(wstr.begin(), wstr.end()).c_str(), &pi, "(ov8u).(ov8u).(ov8u).(ov8u)", &sip[3], &sip[2], &sip[1], &sip[0])){
							throw std::runtime_error("failed to parse ip");
						}

						NET_addr_t net_addr;
						net_addr.port = std::stoi(base->gui.stb.get_line(1, 0));
						net_addr.ip = *(uint32_t*)sip;

						uint64_t secret = std::stoi(base->gui.stb.get_line(2, 0));

						if(!com_view(base, net_addr, secret)){
							/* com_view returns NET_TCP_peer_t * */
							/* 0 pointer means failed to open. how should we handle this? */
							assert(0); /* TODO */
						}

						break;
					}
					case 2:{
						uint16_t port = std::stoi(base->gui.stb.get_line(0, 0));
						uint64_t secret = std::stoi(base->gui.stb.get_line(1, 0));

						VAS_node_t node = com_grabfrom(base, port, secret);
						assert(node != (VAS_node_t)-1);

						break;
					}
					case 3:{
						uint8_t sip[4];
						uint_t pi = 0;

						auto wstr = base->gui.stb.get_line(0, 0);

						if (STR_rscancc(std::string(wstr.begin(), wstr.end()).c_str(), &pi, "(ov8u).(ov8u).(ov8u).(ov8u)", &sip[3], &sip[2], &sip[1], &sip[0])){
							throw std::runtime_error("failed to parse ip");
						}

						NET_addr_t net_addr;
						net_addr.port = std::stoi(base->gui.stb.get_line(1, 0));
						net_addr.ip = *(uint32_t*)sip;

						uint64_t secret = std::stoi(base->gui.stb.get_line(2, 0));

						uint32_t fps = std::stoi(base->gui.stb.get_line(3, 0));

						uint32_t rate = std::stoi(base->gui.stb.get_line(4, 0));

						bool r = com_grabto(base, net_addr, secret, fps, rate);
						assert(!r);

						break;
					}
				}
				break;
			}
		}
	});

	EV_ev_t_init(&base->gui.evt, .2, (EV_ev_cb_t)gui_main_cb);
	EV_ev_t_start(&base->listener, &base->gui.evt);

	EV_start(&base->listener);
}

int main(){
	base_t base;
	run(&base);
	return 0;
}
