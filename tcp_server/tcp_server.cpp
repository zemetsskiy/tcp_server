#undef UNICODE

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <iostream>
#include <stdio.h>
#include <process.h>
#include <map>
#include <fstream>
#include <string>

#pragma comment (lib, "Ws2_32.lib")

#define DEFAULT_BUFLEN 512

int get_config_data(std::string& server_port) {
	std::string filename = "../Debug/tcp_server.cfg.TXT";

	std::ifstream config(filename);
	if (!config.is_open()) {
		std::cerr << "Error: Unable to open config file " << filename << std::endl;
		return 1;
	}

	if (config.peek() == std::ifstream::traits_type::eof()) {
		std::cerr << "Error: Config file " << filename << " is empty" << std::endl;
		return 1;
	}

	std::string line;
	while (std::getline(config, line)) {
		if (line.find("server_port=") == 0) {
			server_port = line.substr(12);
			break;
		}
	}

	config.close();

	//std::cout << "Server port: " << server_port << std::endl;
	return 0;
}

// получаем адрес сокета, ipv4 или ipv6
void* get_in_addr(struct sockaddr* sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// получаем порт сокета
u_short get_in_port(struct sockaddr* sa)
{
	if (sa->sa_family == AF_INET) {
		return ((struct sockaddr_in*)sa)->sin_port;
	}

	return ((struct sockaddr_in6*)sa)->sin6_port;
}

// каждому клиентскому потоку, создаваемому при подключении нового клиента к серверу, соответствует свой сокет
std::map<HANDLE, SOCKET> ClientsSockets;

unsigned __stdcall ClientThreadFunc(void* pArguments)
{
	int iResult, iSendResult;
	char recvbuf[DEFAULT_BUFLEN+1];
	int recvbuflen = DEFAULT_BUFLEN+1;

	// указатель на void используется для хранения указателей на объекты любого типа, без конкретизации их типа на этапе компиляции

	// указатель на void приводится к указателю на SOCKET, затем разыменовываем для получения значения SOCKET
	SOCKET ClientSocket = *((SOCKET*)pArguments);

	struct sockaddr_storage client_socket; // информация об адресе клиента
	socklen_t client_socket_len = sizeof(struct sockaddr);
	char s[INET6_ADDRSTRLEN];

	getpeername(ClientSocket, (sockaddr*)&client_socket, &client_socket_len);

	inet_ntop(client_socket.ss_family,
		get_in_addr((struct sockaddr*)&client_socket),
		s, sizeof s);

	do {
		// заполнение буфера recvbuf нулевыми байтами
		memset(recvbuf, 0, DEFAULT_BUFLEN);
		// получение данных от сокета
		iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
		if (iResult > 0) {
			time_t t = time(NULL);
			struct tm tm = *localtime(&t);
			char timestamp[10];
			sprintf(timestamp, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

			printf("%s From %s:%u: %s\n", timestamp, s, ntohs(get_in_port((struct sockaddr*)&client_socket)), recvbuf);
			//std::cout << timestamp << "From :" << s << ":" << ntohs(get_in_port((struct sockaddr*)&client_socket)) << ": "<< recvbuf << std::endl;

			// перебор элементов ClientsSockets, словаря, где ключами являются дескрипторы потоков, а значениями - сокеты
			// отправка сообщения в буфере всем клиентам, но не отправителю
			for (std::map<HANDLE, SOCKET>::const_iterator it = ClientsSockets.begin(); it != ClientsSockets.end(); ++it) {
				if ((*it).second == ClientSocket)
					continue;
				iSendResult = send((*it).second, recvbuf, iResult, 0);
			}
		}
		else if (iResult == 0)
			printf("Connection closing...\n");
		else {
			printf("recv failed with error: %d\n", WSAGetLastError());
			closesocket(ClientSocket);
			WSACleanup();
			return 1;
		}

	} while (iResult > 0);

	iResult = shutdown(ClientSocket, SD_SEND);
	closesocket(ClientSocket);

	_endthreadex(0);
	return 0;
}

int main(int argc, char** argv)
{
	// инициализация библиотеки сокетов


	// переменная типа WSADATA - структура, используется для хранения информации о версии и конфигурации библиотеки винсок
	WSADATA wsaData;
	// makeword определяет версию винсок, которую необходимо использовать
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		std::cerr << "WSAStartup failed: " << iResult << std::endl;
		return 1;
	}

	// структура для представления адреса сокета
	struct addrinfo* result = NULL;
	struct addrinfo hints;

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;


	std::string port;

	if (argc == 1) {
		// нет аргументов
		int res = get_config_data(port);
		if (res == 1) return 1;
	}
	else {
		port = argv[1];
	}


    // переменная result будет содержать связанный список структур addrinfo
	iResult = getaddrinfo(NULL, port.c_str(), &hints, &result);
	if (iResult != 0) {
		std::cerr << "getaddrinfo failed: " << iResult << std::endl;
		// очищаем все ресурсы, связанные с инициализацией сокетов
		WSACleanup();
		return 1;
	}

	SOCKET ListenSocket = INVALID_SOCKET;
	SOCKET ClientSocket = INVALID_SOCKET;

	ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (ListenSocket == INVALID_SOCKET) {
		std::cerr << "socket creating failed: " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		WSACleanup();
		return 1;
	}

	// связываем сокет с адресом
	iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		std::cerr << "bind failed with error: " << WSAGetLastError() << std::endl;

		freeaddrinfo(result);
		closesocket(ListenSocket);
		WSACleanup();
		return 1;
	}

	freeaddrinfo(result);

	// перевода сокета в режим прослушивания соединений на серверной стороне
	iResult = listen(ListenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR) {
		std::cerr << "listen failed with error: " << WSAGetLastError() << std::endl;
		closesocket(ListenSocket);
		WSACleanup();
		return 1;
	}

	printf("server: Waiting for new connections!\n");

	while (1) {
		struct sockaddr_storage client_socket; // информация об адресе клиента
		socklen_t client_socket_len = sizeof(struct sockaddr);
		char s[INET6_ADDRSTRLEN];

		// принимаем входящее соединение от клиента
		ClientSocket = accept(ListenSocket, (struct sockaddr*)&client_socket, &client_socket_len);
		if (ClientSocket == INVALID_SOCKET) {
			std::cerr << "accept failed with error: " << WSAGetLastError() << std::endl;
			closesocket(ListenSocket);
			WSACleanup();
			return 1;
		}

		inet_ntop(client_socket.ss_family,
			get_in_addr((struct sockaddr*)&client_socket),
			s, sizeof s);
		printf("server: got connection from %s : %u\n", s, ntohs(get_in_port((struct sockaddr*)&client_socket)));


		// создание нового потока для обработки подключения клиента к сокету
		HANDLE hThread;
		unsigned threadID;

		// приведение к типу HANDLE используется для явного указания, что возвращаемое значение функции _beginthreadex является дескриптором (handle) нового потока
		hThread = (HANDLE)_beginthreadex(NULL, 0, &ClientThreadFunc, (void*)(&ClientSocket), 0, &threadID);

		ClientsSockets[hThread] = ClientSocket;
	}

	closesocket(ListenSocket);
	WSACleanup();

	return 0;
}