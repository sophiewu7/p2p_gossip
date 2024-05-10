#include "process.h"
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <thread>
#include <vector>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <random> 
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <algorithm>


void safeJoin(std::thread& th) {
    if (th.joinable()) {
        std::thread::id thisId = std::this_thread::get_id();
        if (th.get_id() != thisId) {
            th.join();
        } else {
            th.detach();
        }
    }
}


P2PServer::P2PServer(int index, int n, int tcpPort) 
    : index(index), n(n), tcpPort(tcpPort), udpPort(ROOT_ID + index), tcpSocket(-1), udpSocket(-1), running(false) {
    database[udpPort] = DatabaseEntry();
}


P2PServer::~P2PServer() {
    std::cout << "## P2PServer::~P2PServer destructor called" << std::endl;
    shutdownServer();
}


void P2PServer::start() {
    running.store(true);
    proxyThread = std::thread(&P2PServer::proxyCommunication, this);
    neighborThread = std::thread(&P2PServer::neighborCommunication, this);
    statusBroadcastThread = std::thread(&P2PServer::broadcastStatusPeriodically, this);
}


void P2PServer::shutdownServer() {
    std::cout << "## P2PServer::shutdownServer()" << std::endl;

    running.store(false);

    if (tcpSocket >= 0) {
        close(tcpSocket);
        tcpSocket = -1;
    }

    if (udpSocket >= 0) {
        close(udpSocket);
        udpSocket = -1;
    }

    cv.notify_all();

    safeJoin(proxyThread);
    safeJoin(neighborThread);
    safeJoin(statusBroadcastThread);
}


void P2PServer::proxyCommunication() {
    initializeTCPConnection();
    while (running.load()) {
        acceptTCPConnections();
    }
}


void P2PServer::initializeTCPConnection() {
    tcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpSocket < 0) {
        std::cerr << "Failed to create TCP socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(tcpPort);
    
    if (bind(tcpSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Failed to bind TCP socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (listen(tcpSocket, MAX_PEERS) < 0) {
        std::cerr << "Failed to listen on socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    std::cout << "## P2PServer::initializeTCPConnection() is listening on port " << tcpPort << std::endl;
}


void P2PServer::acceptTCPConnections() {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    while (running.load()) {
        int clientSocket = accept(tcpSocket, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (clientSocket < 0) {
            std::cerr << "Failed to accept connection." << std::endl;
            continue;
        }
        handleTCPConnection(clientSocket);
    }
}


void P2PServer::handleTCPConnection(int clientSocket) {
    std::string receivedData;
    char buffer[MAX_BUFFER_SIZE];
    bool keepListening = true;

    while (keepListening) {
        ssize_t bytesRead = read(clientSocket, buffer, MAX_BUFFER_SIZE - 1);
        if (bytesRead <= 0) {
            break;
        }
        buffer[bytesRead] = '\0';
        receivedData.append(buffer);
        size_t pos;
        while ((pos = receivedData.find('\n')) != std::string::npos) {
            std::string command = receivedData.substr(0, pos);
            receivedData.erase(0, pos + 1);
            keepListening = processCommand(command, clientSocket);
            if (!keepListening) {
                break;
            }
        }
    }
    if (clientSocket >= 0) {
        close(clientSocket);
    }
}


bool P2PServer::processCommand(const std::string& command, int clientSocket){
    if (command.substr(0, 4) == "msg ") {
        try {
            size_t messageIdEnd = command.find(' ', 4);
            if (messageIdEnd != std::string::npos) {
                std::string messageText = command.substr(messageIdEnd + 1);
                storeMessage(messageText);
            }
        } catch (const std::invalid_argument& e) {
            std::cerr << "Invalid argument for stoi: " << e.what() << '\n';
        } catch (const std::out_of_range& e) {
            std::cerr << "Argument for stoi out of range: " << e.what() << '\n';
        } 
        printAllMessages();
        return true;
    } else if (command == "get chatLog") {
        std::string chatLog = compileChatLog();
        if (chatLog.empty() || chatLog == "chatLog") {
            std::string message = "chatLog <Empty>\n";
            send(clientSocket, message.c_str(), message.size(), 0);
        } else {
            chatLog += "\n";
            size_t totalSent = 0;
            while (totalSent < chatLog.size()) {
                ssize_t sent = send(clientSocket, chatLog.c_str() + totalSent, chatLog.size() - totalSent, 0);
                if (sent < 0) {
                    std::cerr << "Error sending chat log: " << strerror(errno) << std::endl;
                    break;
                }
                totalSent += sent;
            }
        }
        return true;
    } else if (command == "crash") {
        shutdownServer();
        return false;
    } else {
        std::cerr << "Unknown command received: " << command << std::endl;
        return true;
    }
}


void P2PServer::storeMessage(const std::string& messageText) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    DatabaseEntry& entry = database[udpPort];
    entry.messages[entry.lowestSeqNum] = messageText;
    sendRumorMessage(udpPort, messageText, entry.lowestSeqNum);
    entry.lowestSeqNum++;
}


bool P2PServer::sendUDPMessage(int receiverPort, const std::string& message) {

    int sockfd;
    struct sockaddr_in receiverAddr;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        std::cerr << "Socket creation failed: " << strerror(errno) << std::endl;
        return false;
    }

    memset(&receiverAddr, 0, sizeof(receiverAddr));
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port = htons(receiverPort);
    receiverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (sendto(sockfd, message.c_str(), message.length(), 0, (struct sockaddr *)&receiverAddr, sizeof(receiverAddr)) < 0) {
        std::cerr << "Failed to send message: " << strerror(errno) << std::endl;
        close(sockfd);
        return false;
    }

    close(sockfd);
    return true;
}


void P2PServer::sendRumorMessage(int messageOwner, const std::string& messageText, int seqnum) {
    int receiverPort = P2PServer::pickANeighbor(-1);
    if (receiverPort != -1){
        std::string message = constructRumorMessage(messageOwner, messageText, seqnum);
        sendUDPMessage(receiverPort, message);
    } else {
        std::cout << "## P2PServer::sendRumorMessage no neighbor to talk to" << std::endl;
    }
}


std::string P2PServer::constructRumorMessage(int originPort, const std::string& messageText, int seqnum) {
    std::stringstream message;
    message << "rumor:" << udpPort << ":{" << messageText << "," << originPort << "," << seqnum << "}";
    return message.str();
}


std::vector<int> P2PServer::getNeighbors() {
    std::vector<int> neighbors;
    if (index == 0) {
        neighbors.push_back(udpPort+1);
    }
    else if (index == n - 1) {
        neighbors.push_back(udpPort-1);
    }
    else if (index > 0 && index < n - 1) {
        neighbors.push_back(udpPort - 1);
        neighbors.push_back(udpPort + 1);
    }
    return neighbors;
}


int P2PServer::pickANeighbor(int excludePort = -1) {
    std::vector<int> neighbors = getNeighbors();
    if (excludePort != -1) {
        neighbors.erase(std::remove(neighbors.begin(), neighbors.end(), excludePort), neighbors.end());
    }

    if (neighbors.empty()) {
        // std::cerr << "No neighbors to pick from." << std::endl;
        return -1;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(0, neighbors.size() - 1);

    return neighbors[distr(gen)];
}


std::string P2PServer::compileChatLog() {
    std::stringstream chatLogStream;

    chatLogStream << "chatLog " ;

    for (const auto& dbEntryPair : database) {
        for (const auto& messagePair : dbEntryPair.second.messages) {
            chatLogStream << messagePair.second << ",";
        }
    }

    std::string chatLogString = chatLogStream.str();

    if (!chatLogString.empty()) {
        chatLogString.pop_back();
    }

    return chatLogString;
}


void P2PServer::printAllMessages() const {
    std::cout << "------ All Stored Messages ------" << std::endl;
    for (const auto& ownerEntry : database) {
        const auto& ownerUdpPort = ownerEntry.first;
        const auto& databaseEntry = ownerEntry.second;
        std::cout << "Owner UDP Port: " << ownerUdpPort << std::endl;
        
        for (const auto& messageEntry : databaseEntry.messages) {
            const auto& seqNum = messageEntry.first;
            const auto& messageText = messageEntry.second;
            std::cout << "  Seq Num: " << seqNum << ", Message Text: " << messageText << std::endl;
        }
    }
    std::cout << "--------------------------------" << std::endl;
}


void P2PServer::neighborCommunication() {
    initializeUDPConnection();
    std::cout << "## P2PServer::neighborCommunication udpSocket: " << udpSocket << " udpPort: " << udpPort << std::endl;
    
    struct sockaddr_in cliaddr;
    memset(&cliaddr, 0, sizeof(cliaddr));
    socklen_t len = sizeof(cliaddr);
    char buffer[MAX_BUFFER_SIZE];

    while (running.load()) {
        ssize_t bytesRead = recvfrom(udpSocket, (char *)buffer, MAX_BUFFER_SIZE, 0, (struct sockaddr *) &cliaddr, &len);
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0'; // Ensure null-termination
            std::string message(buffer);
            handleGossipMessage(message);
        }
    }
}


void P2PServer::handleGossipMessage(const std::string& message) {
    if (message.compare(0, 6, "rumor:") == 0) {
        handleRumorMessage(message);
    } 
    else if (message.compare(0, 7, "status:") == 0) {
        handleStatusMessage(message);
    } else {
        std::cerr << "Unknown message type: " << message << std::endl;
    }
}


void P2PServer::handleRumorMessage(const std::string& message) {
    std::istringstream messageStream(message);
    std::string segment;
    std::vector<std::string> segments;

    while (std::getline(messageStream, segment, ':')) {
        segments.push_back(segment);
    }

    int receiverPort = std::stoi(segments[1]);

    // Assuming the format is always correct and segments[2] is "{messageText,ownerPort,seqnum}"
    std::string content = segments[2].substr(1, segments[2].size() - 2); // Remove '{' and '}'
    std::replace(content.begin(), content.end(), ',', ':'); // Replace ',' with ':' for uniform splitting
    std::istringstream contentStream(content);
    std::vector<std::string> contentSegments;

    while (std::getline(contentStream, segment, ':')) {
        contentSegments.push_back(segment);
    }

    std::string messageText = contentSegments[0];
    int ownerPort = std::stoi(contentSegments[1]);
    int seqnum = std::stoi(contentSegments[2]);

    if (database.find(ownerPort) == database.end()) {
        database[ownerPort] = DatabaseEntry();
    }

    auto& dbEntry = database[ownerPort];
    if (dbEntry.messages.find(seqnum) == dbEntry.messages.end()) {
        dbEntry.messages[seqnum] = messageText;
        if (seqnum == dbEntry.lowestSeqNum) {
            dbEntry.lowestSeqNum++;
            while (dbEntry.messages.find(dbEntry.lowestSeqNum) != dbEntry.messages.end()) {
                dbEntry.lowestSeqNum++;
            }
        }
    }

    std::string statusMessage = constructStatusMessage(ownerPort);
    sendUDPMessage(receiverPort, statusMessage);
}


std::string P2PServer::constructStatusMessage(int ownerPort) {
    std::stringstream message;
    message << "status:" << udpPort << ":{";
    
    // Check if the owner exists in the database and add if not
    if (database.find(ownerPort) == database.end()) {
        database[ownerPort] = DatabaseEntry();
    }

    DatabaseEntry& entry = database[ownerPort];
    message << ownerPort << ":" << entry.lowestSeqNum;

    for (const auto& dbEntry : database) {
        int port = dbEntry.first;
        if (port != ownerPort) {
            message << "," << port << ":" << dbEntry.second.lowestSeqNum;
        }
    }

    message << "}";
    return message.str();
}


void P2PServer::handleStatusMessage(const std::string& message){

    auto statusPos = message.find("status:") + 7;
    auto bracePos = message.find("{");
    
    std::string portStr = message.substr(statusPos, bracePos - statusPos);
    int receiverPort = std::stoi(portStr);

    auto start = message.find("{") + 1;
    auto end = message.find("}", start);
    std::string statusContent = message.substr(start, end - start);
    std::istringstream contentStream(statusContent);
    std::string pair;
    std::vector<std::pair<int, int>> statusPairs;

    while (std::getline(contentStream, pair, ',')) {
        std::istringstream pairStream(pair);
        std::string part;
        std::getline(pairStream, part, ':');
        int ownerUdpPort = std::stoi(part);
        std::getline(pairStream, part, ':');
        int theirSeqNum = std::stoi(part);
        statusPairs.emplace_back(ownerUdpPort, theirSeqNum);
    }

    /* 1. Check if first owner's message is aligned
        case 1: sender's seq == mine: jump to 2.
        case 2: sender's seq < mine: send rumor to sender for this msg
        case 3: sender's seq > mine: send my status msg with this owner port as origin back to sender to requst for a rumor
       2. First owner all aliged, find if there are other information in the statusPairs where it has a larger seq number than what I have in my database or they have a port that I don't have, if the status msg for this port is 1, create an entry for this port in my database, don't send status msg, else use this as the origin port and construct a status msg to send back to the sender
       3. Every thing aligned, flip a coin to decide whether to stop gossip or pick a new neighbor
    */

    bool databaseAligned = true;
    for (const auto& pair : statusPairs) {
        int ownerPort = pair.first;
        int theirSeqNum = pair.second;

        auto dbIt = database.find(ownerPort);
        if (dbIt != database.end()) {
            int mySeqNum = dbIt->second.lowestSeqNum;
            if (mySeqNum < theirSeqNum) {
                // std::cout << "I am " << udpPort << ". Why you (" << receiverPort << ") requesting from me, you have more than I do, I want to know about " << ownerPort << std::endl;
                std::string statusMessage = constructStatusMessage(ownerPort);
                sendUDPMessage(receiverPort, statusMessage);
                databaseAligned = false; 
                return;
            } else if (mySeqNum > theirSeqNum) {
                // std::cout << "I am " << udpPort << ". I do have the gossip you want to here, here you go the message with seq:" << theirSeqNum << " don't thank me port " << receiverPort << std::endl;
                sendMissingMessages(receiverPort, ownerPort, theirSeqNum);
                databaseAligned = false;
                return;
            }
        } else {
            std::lock_guard<std::mutex> lock(databaseMutex);
            database[ownerPort] = DatabaseEntry();
            if (theirSeqNum > database[ownerPort].lowestSeqNum){
                // std::cout << "I am " << udpPort << ". you have gossip for " << ownerPort << " I wanna know! tell me port " << receiverPort << std::endl;
                std::string statusMessage = constructStatusMessage(ownerPort);
                sendUDPMessage(receiverPort, statusMessage);
                databaseAligned = false;
                return;
            }
        }
    }

    if (databaseAligned) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 1);
        if (dis(gen) == 0) {
            int newReceiverPort = pickANeighbor(receiverPort);
            if (newReceiverPort != -1){
                std::string statusMessage = constructStatusMessage(udpPort);
                sendUDPMessage(newReceiverPort, statusMessage);
            }
        }
    }
}


void P2PServer::sendMissingMessages(int receiverPort, int originPort, int neededSeqNum) {
    auto dbEntryIt = database.find(originPort);
    if (dbEntryIt == database.end()) {
        std::cerr << "No database entry found for port " << originPort << std::endl;
        return;
    }

    const auto& messages = dbEntryIt->second.messages;
    auto messageIt = messages.find(neededSeqNum);
    if (messageIt != messages.end()) {
        std::string rumorMessage = constructRumorMessage(originPort, messageIt->second, messageIt->first);
        sendUDPMessage(receiverPort, rumorMessage);
    }
}


void P2PServer::initializeUDPConnection() {
    struct sockaddr_in udpAddr;
    if ((udpSocket = socket(AF_INET, SOCK_DGRAM, 0))< 0) {
        std::cerr << "Failed to create UDP socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    memset(&udpAddr, 0, sizeof(udpAddr));
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_addr.s_addr = INADDR_ANY;
    udpAddr.sin_port = htons(udpPort);

    if (bind(udpSocket, (const struct sockaddr *)&udpAddr, sizeof(udpAddr)) < 0) {
        std::cerr << "Failed to bind UDP socket." << std::endl;
        exit(EXIT_FAILURE);
    }
}


void P2PServer::waitForThreadsToFinish() {
    if (proxyThread.joinable()) {
        proxyThread.join();
    }
    if (neighborThread.joinable()) {
        neighborThread.join();
    }
    if (statusBroadcastThread.joinable()){
        statusBroadcastThread.join();
    }
}


void P2PServer::broadcastStatusPeriodically() {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Wait for 5 seconds
        broadcastStatusToNeighbors();
    }
}


void P2PServer::broadcastStatusToNeighbors(){
    std::vector<int> neighbors = getNeighbors();
    std::string statusMessage = constructStatusMessage(udpPort);

    for (int neighborPort : neighbors) {
        sendUDPMessage(neighborPort, statusMessage);
    }
}


int main(int argc, char* argv[]) {

    /* Check Arguments */
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << "<pid> <n> <port>" << std::endl;
        return EXIT_FAILURE;
    }

    int index = std::atoi(argv[1]);
    int n = std::atoi(argv[2]);
    int tcpPort = std::atoi(argv[3]);

    /* Start Server */
    P2PServer server(index, n, tcpPort);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.waitForThreadsToFinish();

    return EXIT_SUCCESS;
}