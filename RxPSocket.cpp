//
// Created by alchaussee on 4/8/15.
//

#include <cstring>
#include <unistd.h>

#include "RxPException.h"

#include "RxPSocket.h"

#include <iostream>
#include <arpa/inet.h>

#include "RxPMessage.h"

using namespace std;
using namespace std::chrono;

RxPSocket::RxPSocket(RxPSocket &&sock) {
  _out_buffer = move(sock._out_buffer);
  _in_buffer = move(sock._in_buffer);
  _in_thread = move(sock._in_thread);
  _out_thread = move(sock._out_thread);
  _handle = sock._handle;
  sock._handle = 0;
  _destination_info = sock._destination_info;
  _connected = sock._connected;
  _destination_seq_num = sock._destination_seq_num;
  _seq_num = sock._seq_num;
  _window_size = 1;
  _verbose = sock._verbose;
}

RxPSocket RxPSocket::listen(int local_port) {
  RxPSocket sock;

  // bind to local port
  sock.debug_msg("bound to " + to_string(local_port));
  sock._local_port = local_port;
  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(local_port);

  if (bind(sock._handle, (struct sockaddr *) &address, sizeof(address)) < 0)
    throw RxPException(errno);

  // use recvFrom to listen for connection requests
  RxPMessage message;
  struct sockaddr_in senderInfo;
  socklen_t addrlen = sizeof(senderInfo);
  sock.debug_msg("waiting for syn");
  do {
    try {
      message.parseFromBuffer(sock.receiveFrom(senderInfo, addrlen));
    } catch(const RxPMessage::ParseException& e) {
      continue;
    }
  } while(!message.SYN_flag);
  cout << "Got syn" << endl;

  // save sender info and initiate synchronization handshake
  sock._destination_info = senderInfo;
  sock.debug_msg("got syn sending syn ack and waiting for ack");
  do {
    RxPMessage sendmessage;
    sendmessage.SYN_flag = true;
    sendmessage.ACK_flag = true;
    sendmessage.ACK_number = message.sequence_number + 1;
    sendmessage.sequence_number = sock._seq_num;
    sendmessage.fillChecksum();
    vector<char> buffer = sendmessage.toBuffer();
    cout << "Sending syn ack" << endl;
    sock.sendTo(buffer.data(), buffer.size(), sock._destination_info, sizeof(sock._destination_info));
    try {
      cout << "Waiting for ack" << endl;
      message.parseFromBuffer(sock.receiveFrom(senderInfo, addrlen));
      cout << "Got a message" << endl;
    } catch (const RxPMessage::ParseException &e) {
      sock.debug_msg(e.what());
      continue;
    }
    //Got a message (maybe last ack and if not) check if get some other message using sequence number
    if(message.sequence_number>sock._seq_num)
    {
      cout << "Ending handshake due to higher sequence number" << endl;
      break;
    }

  } while(!(message.ACK_flag && message.ACK_number == sock._seq_num));
  cout << "Got ack handshake done" << endl;
  // initialize buffers / resources
  sock.init();

  return sock;
}

RxPSocket RxPSocket::connect(string ip_address, int foreign_port, int local_port) {
  RxPSocket sock;

  // bind to local port, if set
  sock.debug_msg("bound to " + to_string(local_port));
  sock.debug_msg("Sending to " + to_string(foreign_port));
  if(local_port != -1)
  {
    sock._local_port = local_port;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(local_port);
    if(bind(sock._handle, (struct sockaddr *)&address, sizeof(address)) < 0)
      throw RxPException(errno);
  } else {
    struct sockaddr_in info;
    socklen_t len = sizeof(info);
    if(getsockname(sock._handle, (struct sockaddr *)&info, &len) < 0)
      throw RxPException(errno);
    sock._local_port = ntohs(info.sin_port);
  }

  // send connection request to specified server
  sock._destination_info.sin_family = AF_INET;
  sock._destination_info.sin_port = htons(foreign_port);
  if(inet_pton(AF_INET, ip_address.c_str(), &(sock._destination_info.sin_addr)) <= 0)
    throw RxPException(errno);
  RxPMessage response_message;
  struct sockaddr_in senderInfo;
  socklen_t addrlen = sizeof(senderInfo);
  sock.debug_msg("Sending syn");
  do {
    RxPMessage send_message;
    send_message.SYN_flag = true;
    send_message.ACK_flag = false;
    send_message.sequence_number = sock._seq_num;
    send_message.fillChecksum();
    vector<char> buffer = send_message.toBuffer();
    cout << "Sending syn" << endl;
    sock.sendTo(buffer.data(), buffer.size(), sock._destination_info, sizeof(sock._destination_info));
    sock.debug_msg("Waiting for syn ack");
    try {
      cout << "Waiting for syn ack" << endl;

      response_message.parseFromBuffer(sock.receiveFrom(senderInfo, addrlen));
    } catch(const RxPMessage::ParseException &e) {
      sock.debug_msg("Received invalid message. Resending.");
      continue;
    }
  } while(!(response_message.ACK_flag && response_message.SYN_flag));
  cout << "Got syn ack" << endl;
  // complete synchronization handshake
  RxPMessage ack_message;
  ack_message.ACK_flag = true;
  ack_message.SYN_flag = false;
  ack_message.ACK_number = response_message.sequence_number + 1;
  ack_message.sequence_number = sock._seq_num;
  ack_message.fillChecksum();
  vector<char> buffer = ack_message.toBuffer();
  cout << "Sending ack" << endl;
  sock.sendTo(buffer.data(), buffer.size(), sock._destination_info, sizeof(sock._destination_info));

  // initialize buffers / resources
  sock.init();

  return sock;
}

int RxPSocket::recv(char *buffer, int buffer_length) {
  lock_guard<mutex> lock(_in_mutex);
  int numBytesToCopy = min(buffer_length, (int)_in_buffer.size());
  copy(_in_buffer.begin(), _in_buffer.begin() + numBytesToCopy, buffer);
  return numBytesToCopy;
}

int RxPSocket::send(char *buffer, int buffer_length, int timeout) {
  lock_guard<mutex> lock(_out_mutex);
  int numBytesToCopy = min(buffer_length, (int)(_out_buffer.capacity() - _out_buffer.size()));
  copy(buffer, buffer + numBytesToCopy, _out_buffer.begin());
  return numBytesToCopy;
}

void RxPSocket::close() {
  ::close(_handle);
}

RxPSocket::RxPSocket() {
  _handle = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if(_handle < 0)
    throw RxPException(errno);
  _connected = false;
  _destination_seq_num = 0;
  _seq_num = 0;
  _verbose = true;
}

void RxPSocket::init() {
  _connected = true;
  _in_buffer.reserve(10000);
  _out_buffer.reserve(10000);
  _in_thread = thread(&RxPSocket::in_process, this);
  _out_thread = thread(&RxPSocket::out_process, this);
}

vector<char> RxPSocket::receiveFrom(struct sockaddr_in &senderInfo, socklen_t &senderLength) {
  char buffer[111];
  senderLength = sizeof(senderInfo);
  auto bytesrecvd = recvfrom(_handle, buffer, 110, 0, (struct sockaddr *)&senderInfo, &senderLength);
  if(bytesrecvd < 0)
    throw RxPException(errno);
  buffer[bytesrecvd] = 0;
  vector<char> result(buffer, buffer + bytesrecvd);
  return result;
}

void RxPSocket::sendTo(const char *buffer, int length, const struct sockaddr_in &receiver,
                       const socklen_t &receiverLength) {
  if(sendto(_handle, buffer, length, 0, (struct sockaddr *)&receiver, receiverLength) < 0)
    throw RxPException(errno);
}

void RxPSocket::setWindowSize(int size)
{
  _window_size = size;
}

int RxPSocket::getWindowSize()
{
  return _window_size;
}

/*
 * If there is space left in the in_buffer, use recvFrom to fill that space
 * Managing window size stuff
 */
void RxPSocket::in_process()
{
  int prev_seq_num = 0;
  while(_connected)
  {
    if(_in_buffer.capacity()-_in_buffer.size() > DATASIZE)
    {
      RxPMessage msg;
      struct sockaddr_in senderInfo;
      socklen_t addrlen = sizeof(senderInfo);
      try {
        msg.parseFromBuffer(receiveFrom(senderInfo, addrlen));
      } catch(const RxPMessage::ParseException &e) {
        continue;
      } catch(const RxPException &e) {
        cerr << "Socket exception occured: " << e.what() << endl;
      }

      // Ignore out of order packets
      if(prev_seq_num > msg.sequence_number)
      {
        continue;
      }

      //Duplicate packet
      if(prev_seq_num == msg.sequence_number)
        continue;

      if(msg.ACK_flag)
      {
        lock_guard<mutex> lock(_out_mutex);
        _seq_num = msg.ACK_number;
        _out_buffer.erase(_out_buffer.begin(), _out_buffer.begin() + (_seq_num - _out_buffer_start_seq));
        _out_buffer_start_seq = _seq_num;
        _ack_received = true;
      } else if(msg.FIN_flag) {
        // TODO handle FIN messages
      } else { // No flags means data message
        _in_buffer.insert(_in_buffer.end(), msg.data.begin(), msg.data.end());

        RxPMessage ackMsg;
        ackMsg.ACK_number = msg.sequence_number;
        ackMsg.ACK_flag  = true;
        ackMsg.fillChecksum();

        vector<char> buffer = msg.toBuffer();
        sendTo(buffer.data(), buffer.size(), _destination_info, sizeof(_destination_info));
      }

      setWindowSize(_in_buffer.capacity()-_in_buffer.size()/DATASIZE);
      prev_seq_num = msg.sequence_number;
    }

  }
}

void RxPSocket::out_process()
{
  while(_connected)
  {
    int init_seq_num = _seq_num;
    { // Scope for mutex lock
      lock_guard<mutex> lock(_out_mutex);
      if(_out_buffer.empty())
        continue;
      //Send multiple messages
      auto iter = _out_buffer.begin();
      for (int i = 0; i < _window_size && _seq_num < (_out_buffer_start_seq + _out_buffer.size()); i++)
      {
        RxPMessage msg;
        msg.dest_port = _destination_info.sin_port;
        msg.src_port = _local_port;
        msg.sequence_number = _seq_num;

        auto numBytesToSend = min(_seq_num - _out_buffer_start_seq, DATASIZE);
        msg.data = vector<char>(_out_buffer.begin() + (_seq_num - _out_buffer_start_seq), _out_buffer.begin() +  + (_seq_num - _out_buffer_start_seq) + numBytesToSend);

        _seq_num += numBytesToSend;

        msg.fillChecksum();

        vector<char> buffer = msg.toBuffer();
        try {
          sendTo(buffer.data(), buffer.size(), _destination_info, sizeof(_destination_info));
        } catch(const RxPException &e) {
          cerr << "Socket exception occurred: " << e.what() << endl;
          _seq_num -= numBytesToSend;
          i--;
        }
      }
    }
    auto start_time = system_clock::now();
    auto timeout_duration = duration<int>(10); // 10 seconds
    while(!_ack_received)
    {
      this_thread::__sleep_for(seconds(1), nanoseconds(0));
      if( (system_clock::now() - start_time) > timeout_duration )
        break;
    }
    if(!_ack_received)
    {
      // Ack never received so must've timed out.
      _seq_num = init_seq_num;
      continue;
    }
  }
}

void RxPSocket::debug_msg(string msg) {
  if(_verbose)
    cout << msg << endl;
}
