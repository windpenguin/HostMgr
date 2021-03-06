//
//	<one line to give the program's name and a brief idea of what it does.>
//	Copyright (C) 2017. WenJin Yu. windpenguin@gmail.com.
//
//	Created at 2017/9/6 13:39:10
//	Version 1.0
//
//	This program is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#pragma once

#include <boost/asio.hpp>
#include <json/json.h>
#include <deque>
#include <Base/Msg.h>
#include <Base/SingleQueue/SingleQueue.h>
#include <Base/Log/Log.h>
#include <thread>

namespace wind {

using boost::asio::ip::tcp;
using namespace std;

class Client {
public:
	enum class EConnectState { None, Connecting, Fail, Ok };
	enum class ELoginState { None, Logging, Fail, Ok };

public:
	Client(boost::asio::io_service& io_service, tcp::resolver::iterator endpoint_iterator)
		: io_service_(io_service)
		, socket_(io_service)
		, connectState_(EConnectState::None)
		, loginState_( ELoginState::None )
		, outMsgs_(1000)
		, inMsgs_(1000)
		, bNetWriteMsg_(false)
		, stop_(false)
		, runProcessMsg_(false)
	{
		ConnectServer(endpoint_iterator);
	}

	~Client() {
		Stop();
	}

	Client::EConnectState ConnectState() { return connectState_; }
	bool ConnectOk() { return connectState_ == EConnectState::Ok; }
	Client::ELoginState LoginState() { return loginState_; }
	bool LoginOk() { return loginState_ == ELoginState::Ok; }

	void SendMsg(const Msg& msg) {
		if (!LoginOk()) {
			EASY_LOG_FILE("main") << "SendMsg not login";
			return;
		}

		EASY_LOG_FILE("main") << "SendMsg:" << msg.Body();
		WriteMsg(msg);
	}

	bool QueryCmd() {
		if (!LoginOk()) {
			EASY_LOG_FILE("main") << "QueryCmd not login";
			return false;
		}

		Json::Value val;
		val["msgType"] = static_cast<int>(Msg::MsgType::QueryCmd);
		SendJsonMsg(val);

		return true;
	}

	bool SendCmd(int opt) {
		if (!LoginOk()) {
			EASY_LOG_FILE("main") << "SendCmd not login";
			return false;
		}

		Json::Value val;
		val["msgType"] = static_cast<int>(Msg::MsgType::Cmd);
		val["opt"] = opt;
		SendJsonMsg(val);

		return true;
	}

	bool Login(string user, string pwd) {
		if (!ConnectOk())
			return false;

		loginState_ = ELoginState::Logging;

		Json::Value val;
		val["msgType"] = static_cast<int>(Msg::MsgType::Login);
		val["user"] = user;
		val["pwd"] = pwd;
		SendJsonMsg(val);

		return true;
	}

	bool Logout() {
		if (!LoginOk()) {
			EASY_LOG_FILE("main") << "Logout not login";
			return false;
		}

		Json::Value val;
		val["msgType"] = static_cast<int>(Msg::MsgType::Logout);
		SendJsonMsg(val);

		Stop();

		return true;
	}

	void Stop() {
		if (stop_)
			return;
		stop_ = true;

		EASY_LOG_FILE("main") << "Stop client";
		io_service_.post([this]() { 
			if (socket_.is_open()) { 
				EASY_LOG_FILE("main") << "Close socket"; 
				boost::system::error_code ec;
				socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
				socket_.close(); 
			} 
		});
		connectState_ = EConnectState::None;
		loginState_ = ELoginState::None;
		if( runProcessMsg_ )
			thProcessMsg_.join();
	}

private:
	void ConnectServer(tcp::resolver::iterator endpoint_iterator) {
		EASY_LOG_FILE("main") << "Try connect server";
		connectState_ = EConnectState::Connecting;

		boost::asio::async_connect(socket_, endpoint_iterator, 
			[this](boost::system::error_code ec, tcp::resolver::iterator) {
			if (!ec) {
				connectState_ = EConnectState::Ok;
				EASY_LOG_FILE("main") << "Connect succ";

				ReadMsgHeader();

				std::thread t1(std::bind(&Client::ProcessMsg, this));
				thProcessMsg_.swap(t1);
			}
			else {
				EASY_LOG_FILE("main") << "Connect server fail";
				Stop();
			}
		});
	}

	bool SendJsonMsg(Json::Value val) {
		Json::FastWriter writer;
		string strMsgBody = writer.write(val);

		Msg msg;
		msg.SetBodyLength(strMsgBody.length());
		memcpy(msg.Body(), strMsgBody.c_str(), msg.BodyLength());
		msg.EncodeHeader();

		WriteMsg(msg);
		return true;
	}

	void ProcessMsg() {
		runProcessMsg_ = true;
		while (ConnectOk()) {
			MsgRef msgRef;
			while (ConnectOk() && (msgRef = inMsgs_.Read())) {
				Json::Value valMsg;
				Json::Reader reader;
				if (!reader.parse(msgRef->Body(), msgRef->Body() + msgRef->BodyLength(), valMsg)) {
					EASY_LOG_FILE("main") << "ProcessMsg fail parse: " << msgRef->Body();
					continue;
				}

				try {
					switch (valMsg["msgType"].asUInt())
					{
					case Msg::MsgType::LoginAck:
					{
						if (valMsg["result"].asInt() == 0) {
							loginState_ = ELoginState::Ok;
							EASY_LOG_FILE("main") << "Login succ";
						}
						else {
							loginState_ = ELoginState::Fail;
							EASY_LOG_FILE("main") << "Login fail";
						}
					}
					break;
					case Msg::MsgType::CmdAck:
					{
						if (valMsg["result"].asBool() == true) {
							EASY_LOG_FILE("main") << "Do cmd succ";
						}
						else {
							EASY_LOG_FILE("main") << "Do cmd fail";
						}
					}
					break;
					case Msg::MsgType::QueryCmdAck:
					{
						EASY_LOG_FILE("main") << "============================================================";
						EASY_LOG_FILE("main") << "Enter number to send cmd: ";

						auto& valCmds = valMsg["cmds"];
						for (Json::ArrayIndex i = 0; i < valCmds.size(); ++i) {
							Json::Value& valCmd = valCmds[i];

							EASY_LOG_FILE("main") << valCmd["opt"].asInt() << ": " << valCmd["info"].asString();
						}
						EASY_LOG_FILE("main") << "============================================================";
					}
					break;
					default:
						EASY_LOG_FILE("main") << "ProcessMsg unknown: " << msgRef->Body();
						break;
					}
				}
				catch (Json::Exception e)
				{
					EASY_LOG_FILE("main") << "ProcessMsg exception: " << e.what();
				}
				catch (...)
				{
					EASY_LOG_FILE("main") << "ProcessMsg exception: unknown";
				}
				
			}

			Sleep(100);
		}
	}

	void ReadMsgHeader() {
		inMsg_.Clear();
		boost::asio::async_read(socket_, boost::asio::buffer(inMsg_.Data(), Msg::HEADER_LENGTH),
			[this](boost::system::error_code ec, size_t length) {
			if (!ec && inMsg_.DecodeHeader()) {
				ReadMsgBody();
			}
			else {
				EASY_LOG_FILE("main") << "ReadMsgHeader fail";
				Stop();
			}
		});
	}

	void ReadMsgBody() {
		boost::asio::async_read(socket_, boost::asio::buffer(inMsg_.Body(), inMsg_.BodyLength()),
			[this](boost::system::error_code ec, size_t length) {
			if (!ec) {
				EASY_LOG_FILE("main") << "ReadMsg: " << inMsg_.Body();

				inMsgs_.Write(make_shared<Msg>(inMsg_));

				ReadMsgHeader();
			}
			else {
				EASY_LOG_FILE("main") << "ReadMsgBody fail";
				Stop();
			}
		});
	}

	void WriteMsg(const Msg& msg) {
		outMsgs_.Write(make_shared<Msg>(msg));
		
		if (!bNetWriteMsg_) 
			NetWriteMsg();
	}

	void NetWriteMsg() {
		MsgRef msgRef;
		if (msgRef = outMsgs_.Read()) {
			bNetWriteMsg_ = true;
			boost::asio::async_write(socket_, boost::asio::buffer(msgRef->Data(), msgRef->Length()),
				[this](boost::system::error_code ec, size_t length) {
				if (!ec) {
					EASY_LOG_FILE("main") << "WriteMsg succ";
					NetWriteMsg();
				}
				else {
					EASY_LOG_FILE("main") << "WriteMsg fail";
					Stop();
				}
			});
		}
		else {
			bNetWriteMsg_ = false;
		}
	}

private:
	boost::asio::io_service& io_service_;
	tcp::socket socket_;
	Msg inMsg_;
	EConnectState connectState_;
	ELoginState loginState_;
	SingleQueue<Msg> outMsgs_;
	SingleQueue<Msg> inMsgs_;
	bool bNetWriteMsg_;
	thread thProcessMsg_;
	bool runProcessMsg_;
	bool stop_;
};

} // namespace wind