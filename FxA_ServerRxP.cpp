/*
 * CS 3251 - Programming Homework 2
 * FxA Server Application using RxP
 * Matthew Barulic
 * Richard Chaussee
 */

#include <stdlib.h>
#include <iostream>
#include <boost/thread.hpp>

#include "RxPSocket.h"
#include <fstream>

using namespace std;

int port_to_bind;
string ip;
int port_NetEmu;

void parseArgs(int numArgs, const char* args[]);
void handleInput();
void acceptConnection(RxPSocket* sock, bool* isConnecting);

boost::thread inputThread;
boost::mutex m;
enum Cmd {none, terminate, window};
Cmd command = Cmd::none;
int window_size = 1;

int main(int argc, const char* argv[])
{
    parseArgs(argc, argv);

    cout << "calling listen" << endl;
    RxPSocket sock = RxPSocket::listen(port_to_bind);
    cout << "listened" << endl;

    cout << "Server started" << endl;

    inputThread = boost::thread(handleInput);
    boost::thread acceptThread;
    bool isConnecting = false;
    while(true)
    {
        if(!isConnecting)
            acceptThread = boost::thread(acceptConnection, &sock, &isConnecting);

        m.lock();
        switch(command)
        {
            case Cmd::terminate:
                cout << "Stopping Server" << endl;
                m.unlock();
                pthread_cancel(acceptThread.native_handle());
                sock.close();
                exit(EXIT_SUCCESS);
            case Cmd::window:
                cout << "Setting window size to " << window_size << endl;
                sock.setWindowSize(window_size);
                break;
            default:
                break;
        }
        if(command!=Cmd::none)
            command = Cmd::none;
        m.unlock();
    }
}

void acceptConnection(RxPSocket* sock, bool* isConnecting)
{
    *isConnecting = true;

    while(true) {
        char buf[3] = {0};
        int res = sock->recv(buf, 3);
        if (res == 0)
            cout << "Connection shutdown" << endl;
        if (res == -1)
            cout << "Error receiving data" << endl;
        cout << "Got command " << string((char *) buf) << endl;

        if (string((char *) buf).compare("get") == 0) {
            char b[128] = {0};
            fill(begin(b), end(b), 1);
            int bytesrecvd = 0;
            while (bytesrecvd < 128 && find(begin(b), end(b), 0) == end(b)) {
                res = sock->recv(b + bytesrecvd, 128 - bytesrecvd);
                if (res == -1) {
                    cerr << "Error receiving file name " << string((char *) b) << endl;
                    cerr << strerror(errno) << endl;
                    *isConnecting = false;
                    return;
                }
                bytesrecvd += res;
            }
            cout << "File is \"" << string((char *) b) << "\"" << endl;

            ifstream file(string((char *) b), ios::binary | ios::in);
            if (file.is_open()) {
                char s[] = "GodFile";
                int res = sock->send(s, 7);
                if (res == -1) {
                    cout << "Error sending GoodFile" << endl;
                    *isConnecting = false;
                    return;
                }
                file.seekg(0, ios::end);
                int length = file.tellg();
                file.seekg(0, file.beg);
                cout << "Length is " << length << endl;

                char b[10];
                strcpy(b, to_string(length).c_str());
                cout << b << endl;
                res = sock->send(b, 10, 0);
                if (res == -1) {
                    cout << "Error sending file length" << endl;
                    *isConnecting = false;
                    return;
                }

                char *buffer = new char[length];
                file.read(buffer, length);
                int bytessent = 0;
                int l = length;
                while(bytessent<length)
                {
                    res = sock->send(buffer, l);
                    if (res == -1) {
                        cout << "Error sending file" << endl;
                        *isConnecting = false;
                        return;
                    }
                    bytessent+=res;
                    l-=res;
                    buffer+=res;
                }
                file.close();
                cout << "File successfully sent" << endl;
            }
            else {
                cout << "Error opening file probably doesn't exist" << endl;
                char s[] = "BadFile";
                int res = sock->send(s, 7);
                if (res == -1) {
                    cout << "Error sending BadFile" << endl;
                    *isConnecting = false;
                    return;
                }
            }
        }
        else if (string((char *) buf).compare("pst") == 0) {

            char t[7] = {0};
            res = sock->recv(t, 7);
            if (res == -1) {
                cout << "Error receiving file status" << endl;
                *isConnecting = false;
                return;
            }

            if(string((char*)t).compare("GodFile")==0)
            {
                char l[10] = {0};
                int bytesrecvd = 0;
                while(bytesrecvd<10)
                {
                    res = sock->recv(l, 10);
                    if (res == -1) {
                        cout << "Error receiving file length" << endl;
                        *isConnecting = false;
                        return;
                    }
                    bytesrecvd+=res;
                }
                int length = stoi(string((char *)l));
                cout << "File is " << length << " bytes long" << endl;

                char b[128] = {0};
                fill(begin(b), end(b), 1);
                bytesrecvd = 0;
                while (bytesrecvd < 128 && find(begin(b), end(b), 0) == end(b)) {
                    res = sock->recv(b + bytesrecvd, 128 - bytesrecvd);
                    if (res == -1) {
                        cerr << "Error receiving file name " << string((char *) b) << endl;
                        cerr << strerror(errno) << endl;
                        *isConnecting = false;
                        return;
                    }
                    bytesrecvd += res;
                }
                cout << "File is " << string((char *) b) << " " << length << " bytes long" << endl;

                ofstream file("Server_"+string((char *) b), ios::binary);

                char *buffer = new char[length];
                bytesrecvd = 0;
                while(bytesrecvd<length)
                {
                    res = sock->recv(buffer, length);
                    if (res == -1) {
                        cout << "Error receiving file data" << endl;
                        break;
                    }
                    bytesrecvd+=res;
                    file.write(buffer, res);
                    buffer = buffer+res;
                    length-=res;
                }
                file.close();
                cout << "File successfully received" << endl;
            }
            else if(string((char*)t).compare("BadFile")==0)
            {
                cout << "File doesn't exist on client" << endl;
                continue;
            }
        }
        else {
            cout << "Connection lost" << endl;
            *isConnecting = false;
            return;
        }
        *isConnecting = false;
    }
}

void handleInput()
{
    string cmd;
    while(true)
    {
        cin >> cmd;
        if (cmd.compare("terminate") == 0)
        {
            m.lock();
            command = Cmd::terminate;
            m.unlock();
        }
        else if(cmd.compare("window")==0)
        {
            string w;
            cin >> w;
            try
            {
                window_size = stoi(w);
                m.lock();
                command = Cmd::window;
                m.unlock();
            }
            catch(invalid_argument e)
            {
                cout << "Invalid window size" << endl;
            }
        }
        else
            cout << "Unrecognized Command" << endl;
    }
}

void parseArgs(int numArgs, const char* args[])
{
    if(numArgs>4)
    {
        cout << "Too many arguments" << endl;
        exit(EXIT_FAILURE);
    }
    else if(numArgs<4)
    {
        cout << "Not enough arguments" << endl;
        exit(EXIT_FAILURE);
    }
    else
    {
        try
        {
            port_to_bind = stoi(args[1]);
            ip = args[2];
            port_NetEmu= stoi(args[3]);
        }
        catch(...)
        {
            cout << "Invalid parameters" << endl;
            exit(EXIT_FAILURE);
        }
    }
}