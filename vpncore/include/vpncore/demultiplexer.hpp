﻿#pragma once
#include <memory>
#include <deque>
#include <unordered_map>
#include <functional>

#include <boost/asio/spawn.hpp>

#include <boost/container_hash/hash.hpp>
#include <boost/enable_shared_from_this.hpp>

#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/config.hpp>

#include "vpncore/logging.hpp"

#include "vpncore/endpoint_pair.hpp"
#include "vpncore/chksum.hpp"
#include "vpncore/ip_buffer.hpp"
#include "vpncore/tcp_stream.hpp"

#include "vpncore/tuntap.hpp"
using namespace tuntap_service;


namespace avpncore {

	// 分析ip流, 根据ip流的 endpoint_pair
	// 转发到对应的tcp流模块.
	// 如果不存在, 则创建对应的模块.
	class demultiplexer : public boost::enable_shared_from_this<demultiplexer>
	{
		// c++11 noncopyable.
		demultiplexer(const demultiplexer&) = delete;
		demultiplexer& operator=(const demultiplexer&) = delete;

	public:
		demultiplexer(boost::asio::io_context& io_context,
			tuntap& input)
			: m_io_context(io_context)
			, m_input(input)
			, m_abort(false)
		{}

		~demultiplexer()
		{}

		// start working.
		void start()
		{
 			m_io_context.post(std::bind(
 				&demultiplexer::start_work, shared_from_this()));
		}

		void stop()
		{
			m_abort = true;
		}

		void async_accept(tcp_stream* stream)
		{
			m_backlog.push_back(stream);
		}

		using udp_handler = std::function<void(ip_buffer buf)>;
		void accept_udp(udp_handler handler)
		{
			m_udp_handler = handler;
		}

		void write_udp(ip_buffer buffer)
		{
			ip_packet(buffer);
		}

		void remove_stream(const endpoint_pair& pair)
		{
			m_tcp_conntrack.erase(pair);
		}

		std::size_t num_backlog() const
		{
			return m_backlog.size();
		}

	protected:
		void demux_ip_packet(boost::asio::yield_context yield)
		{
			boost::asio::streambuf buffer;
			boost::system::error_code ec;

			while (!m_abort)
			{
				auto bytes = m_input.async_read_some(
					buffer.prepare(64 * 1024), yield[ec]);
				if (ec)
					return;

				buffer.commit(bytes);
				auto buf = boost::asio::buffer_cast<const uint8_t*>(buffer.data());
				auto endp = lookup_endpoint_pair(buf, bytes);

				// 解析不出来的ip包, 直接跳过.
				if (endp.empty())
				{
					buffer.consume(bytes);
					continue;
				}

				if (endp.type_ == ip_tcp)
				{
					auto stream = lookup_stream(endp);
					if (!stream)
					{
						if (!m_backlog.empty())
						{
							stream = m_backlog.back();
							m_backlog.pop_back();
							stream->set_write_ip_handler(
								std::bind(&demultiplexer::ip_packet, shared_from_this(),
									std::placeholders::_1));
							m_tcp_conntrack[endp] = stream;
						}
					}
					if (stream)
						stream->output(buf, bytes);
				}
				else if (endp.type_ == ip_udp)
				{
					if (m_udp_handler)
					{
						ip_buffer ip;
						ip.assign(buf, bytes);
						ip.endp_ = endp;

						m_udp_handler(ip);
					}
				}

				buffer.consume(bytes);
			}
		}

		void ip_packet(ip_buffer buffer)
		{
			const auto& endp = buffer.endp_;
			if (buffer.empty())			// 连接已经销毁, 传过来空包.
			{
				remove_stream(endp);
				return;
			}

			static uint16_t index = 0;

			// 打包ip头.
			uint8_t* p = buffer.data();

			*((uint8_t*)(p + 0)) = 0x45; // version
			*((uint8_t*)(p + 1)) = 0x00; // tos
			*((uint16_t*)(p + 2)) = htons((uint16_t)buffer.len()); // ip length
			*((uint16_t*)(p + 4)) = htons(index++);	// id
			*((uint16_t*)(p + 6)) = 0x00;	// flag
			*((uint8_t*)(p + 8)) = 0x30; // ttl
			*((uint8_t*)(p + 9)) = endp.type_; // protocol
			*((uint16_t*)(p + 10)) = 0x00; // checksum

			*((uint32_t*)(p + 12)) = htonl(endp.src_.address().to_v4().to_ulong()); // source
			*((uint32_t*)(p + 16)) = htonl(endp.dst_.address().to_v4().to_ulong()); // dest

			*((uint16_t*)(p + 10)) = (uint16_t)~(unsigned int)standard_chksum(p, 20);// htons(sum); // ip header checksum

			// 写入tun设备.
			bool write_in_progress = !m_queue.empty();
			m_queue.push_back(buffer);

			if (!write_in_progress)
			{
				boost::asio::spawn(m_io_context, std::bind(
					&demultiplexer::write_ip_packet, shared_from_this(), std::placeholders::_1));
			}
		}

		tcp_stream* lookup_stream(const endpoint_pair& endp)
		{
			auto it = m_tcp_conntrack.find(endp);
			if (it == m_tcp_conntrack.end())
				return nullptr;
			return it->second;
		}

		void start_work()
		{
			auto self = shared_from_this();
			boost::asio::spawn([self, this]
			(boost::asio::yield_context yield)
			{
				demux_ip_packet(yield);
			});

			boost::asio::spawn([self, this]
			(boost::asio::yield_context yield)
			{
				write_ip_packet(yield);
			});
		}

		void write_ip_packet(boost::asio::yield_context yield)
		{
			boost::asio::steady_timer try_again_timer(m_io_context);

			while (!m_queue.empty())
			{
				boost::system::error_code ec;
				auto p = m_queue.front();
				auto bytes_transferred = m_input.async_write_some(
					boost::asio::buffer(p.data(), p.len_), yield[ec]);
				if (ec)
					break;
				if (bytes_transferred == 0)
				{
					try_again_timer.expires_from_now(std::chrono::milliseconds(64));
					try_again_timer.async_wait(yield[ec]);
					if (!ec)
						continue;
					break;
				}
				m_queue.pop_front();
			}
		}


	private:
		boost::asio::io_context& m_io_context;
		tuntap& m_input;
		std::unordered_map<endpoint_pair, tcp_stream*> m_tcp_conntrack;
		std::vector<tcp_stream*> m_backlog;
		std::deque<ip_buffer> m_queue;
		udp_handler m_udp_handler;
		bool m_abort;
	};

}
