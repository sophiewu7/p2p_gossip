#ifndef PROCESS_H
#define PROCESS_H

#include <unordered_map>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>

constexpr int MAX_MESSAGE_SIZE = 200;
constexpr int MAX_MESSAGES = 1000;
constexpr int MAX_PEERS = 4;
constexpr int MAX_BUFFER_SIZE = 1024;
constexpr int ROOT_ID = 40000;


struct DatabaseEntry {
    int lowestSeqNum = 0;
    std::unordered_map<int, std::string> messages; // Key: seqNum, Value: messageText
};


class P2PServer {
private:
    int index;
    int n;
    int tcpPort;
    int udpPort;
    int tcpSocket;
    int udpSocket;
    std::atomic<bool> running;
    std::unordered_map<int, DatabaseEntry> database; // Key: ownerUdpPort, Value: DatabaseEntry
    std::thread proxyThread;
    std::thread neighborThread;
    std::thread statusBroadcastThread;
    std::condition_variable cv;
    std::mutex cv_m;
    std::mutex databaseMutex;

public:
    P2PServer(int index, int n, int tcpPort);
    ~P2PServer();

    void start();
    void shutdownServer();
    void waitForThreadsToFinish();

    void proxyCommunication();
    void initializeTCPConnection();
    void acceptTCPConnections();
    void handleTCPConnection(int clientSocket);
    bool processCommand(const std::string& command, int clientSocket);
    void storeMessage(const std::string& messageText);
    std::string compileChatLog();
    void printAllMessages() const;
    void sendRumorMessage(int messageOwner, const std::string& messageText, int seqnum);
    std::vector<int> getNeighbors();
    int pickANeighbor(int excludePort);
    std::string constructRumorMessage(int originPort, const std::string& messageText, int seqnum);

    void neighborCommunication();
    void initializeUDPConnection();
    bool sendUDPMessage(int receiverPort, const std::string& message);
    void handleGossipMessage(const std::string& message);
    void handleRumorMessage(const std::string& message);
    std::string constructStatusMessage(int ownerPort);
    void handleStatusMessage(const std::string& message);
    void sendMissingMessages(int receiverPort, int originPort, int neededSeqNum);

    void broadcastStatusPeriodically();
    void broadcastStatusToNeighbors();
};

#endif