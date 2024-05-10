# CS 5450: Networked and Distributed Systems Lab 2: Peer to Peer Networking

## Implementation details

To provide P2P service using gossip protocol, we chose to develop our server using C++. To ensure concurrent send and receive, we used multithreading to create three kinds of separate threads apart from the main threads:
1. **ProxyThread:** use TCP to communicate with `proxy.py` to interact with the users
2. **NeighborThread:** use UDP to gossip with neighbor by sending rumor and status messages.
3. **statusBroadcastThread:** to use for anti-entropy with a timer that when timeout, we send out status message to all neighbors using UDP. 

**Files included in Submission:**
- `process.h`: The header file for `process.cpp`. It includes all constant declarations, function declarations, struct declarations, and the declaration of the P2PServer class.
- `process.cpp`: The C++ source file for our actual implementation for the P2P server using gossip protocol.
- `proxy.py`: Provided by the couse for user to interact with the servers.
- `stopall.cpp`: The C++ source file that stop and kill all servers when `proxy.py` receive an EXIT command.
- `Makefile` - A configuration file used by the make build automation tool to compile and link all the programs.


### `proxy.py` Modification

We changed `line 53` in `proxy.py` to
```python=
self.buffer += data.decode('utf-8')
```
**Explanation:** We found that in Python 3, socket.recv() returns data as a bytes object, not a str object. To concatenate it to self.buffer, which is a string, you need to decode data from bytes to a string using the decode() method, which uses UTF-8 encoding by default.

We also believe `line 40` in `proxy.py` should be
```python=
s = l.split(None, 1)
```
**Explanation:** The current implementation of `s = l.split()` causes the chatroom to support only messages that do not contain spaces. Through our Slack interactions with Tingwei (TA), we have become aware that the test cases do not contain spaces; hence, we left this line as it was before. However, we believe it should be changed to `s = l.split(None, 1)` to be more robust.

### Data Structure

We chose to use C++ over C because we want to use a data structure that has an O(1) search complexity for our message storage database. Hence, we believe C++ is more suitable for our needs.

Our database utilizes the `std::unordered_map` container to map integer keys to corresponding message records. 

**The message records structure is defined as follows:**
```cpp=
std::unordered_map<int, DatabaseEntry> database; // Key: ownerUdpPort, Value: DatabaseEntry
```
- **Key (ownerUdpPort)**: Each server has a unique UDP port. To facilitate easy gossiping, we use the server's UDP port as the unique identifier for the owner of the messages, enabling direct access to their message records.
- **Value (DatabaseEntry)**: Structured to store and organize the messages associated with a particular owner.

**The DatabaseEntry structure is defined as:**
```cpp=
struct DatabaseEntry {
    int lowestSeqNum = 0; // Tracks the lowest sequence number among the stored messages
    std::unordered_map<int, std::string> messages; // Key: seqNum, Value: messageText
};
```
- lowestSeqNum: This integer signifies the lowest sequence number present in the database for an owner, enabling quick identification of the starting point of their messages.
- messages: An `std::unordered_map` that maps sequence numbers (seqNum) to the actual message texts (messageText). This allows for efficient message storage, retrieval, and management based on sequence numbers.


**Sequence Number**
We did not use `Message ID` passed from the `proxy` as it is entered by the user and prone to error. We started the sequence number from 0 and increment it by 1 when the server received a new message from `proxy`. For initialization in the database, the lowestSeqNum is initialize to 0, indicating we are looking for message with seqnum of 0.


### Message Structure

#### Rumor Message
- **Format:**
  `rumor:<SenderPort>:{<MessageText>,<OriginPort>,<SequenceNumber>}`
- **Components:**
  - **rumor**: Identifies the message as a Rumor Message.
  - **SenderPort**: The sender of the rumor message. Need this to let the receiver know who to respond to as we are using UDP.
  - **OriginPort**: The UDP port of the node that originated the message. We define whoever received the message from `proxy` as the owner of the message.
  - **MessageText**: The actual text of the message being propagated.
  - **SequenceNumber**: The sequence number of this message text is used by the receiver to compare with its lowest sequence number for this `OriginPort`, serving the purpose of a vector clock.

    
#### Status Message
- **Format:** 
  `status:<OriginPort>:{<OwnerPort1>:<SeqNum1>,<OwnerPort2>:<SeqNum2>,...}`
- **Components:**
  - **status**: Identifies the message as a Status Message.
  - **OriginPort**: The UDP port of the node sending the status update.
  - **OwnerPortN**: The UDP port of a node that has previously sent messages. This part of the message lists all known nodes from which messages have been received.
  - **SeqNumN**: The highest sequence number of the message received from the corresponding `OwnerPortN`. This indicates up to which message the node is synchronized with the message history of `OwnerPortN`.
- The the message owner that the rumor message belongs to will also be the `OwnerPort1` to ensure this message thread is synced before everything else.

### ProxyThread
use TCP to communicate with `proxy.py` to interact with the users

**Functions involved:**
```cpp!
// main function that calls the rest of the functions as helpers
void proxyCommunication();

// for estalish connection with proxy
void initializeTCPConnection();
void acceptTCPConnections();

// listn to proxy and process command
void handleTCPConnection(int clientSocket);

// split the incoming message to identify which command contains: msg, get chatLog, crash
bool processCommand(const std::string& command, int clientSocket);

// for msg command, store the message into the database
void storeMessage(const std::string& messageText);

// helper function that compile chatLog to send to proxy
std::string compileChatLog();

// helper function for printing message stored in database
void printAllMessages() const;

// everytime the server receive a new msg, send a rumor message for this message to its neighbor using UDP
void sendRumorMessage(int messageOwner, const std::string& messageText, int seqnum);

// helper function for construct rumor message
std::string constructRumorMessage(int originPort, const std::string& messageText, int seqnum);

// helper function to get neighbors: UDP_PORT-1, UDP_PORT+1
std::vector<int> getNeighbors();

// helper function that pick a neighbor by random with the option to exclude specific neighbor
int pickANeighbor(int excludePort);

// helper function for send message through UDP connection
bool sendUDPMessage(int receiverPort, const std::string& message);
```

This thread use TCP protocol to communicate with `proxy`. The port used for TCP connection is the port passed by the `proxy`. We suggest you use `port 20000` and onwards for this communication.


### NeighborThread

use UDP to gossip with neighbor by sending rumor and status messages.

We use `40000` as ROOT_ID for UDP_port. Each server is assigned for a UDP port number of `ROOT_ID + index` where `index` is the `pid` passed from `proxy`. Therefore, as long as the `index` is consecutive, the servers would be neighbor of each other despite the `port` passed from `proxy`.


**Functions involved:**
```cpp!
// main function that calls the rest of the functions as helpers
void neighborCommunication();

// initialize UDP connection, listn on udp_port for gossiping
void initializeUDPConnection();

// handle gossip message when receive msg from UDP port, extract and see whether it is a rumor or status message
void handleGossipMessage(const std::string& message);

// handle rumor message, store the message in the database and send out corresponding status message back to the sender
void handleRumorMessage(const std::string& message);

// helper function for construct status message
std::string constructStatusMessage(int ownerPort);

// handle status message and see what rumor message can be send back to the sender
void handleStatusMessage(const std::string& message);

// helper function that finds the message sender is requesting and send it back as rumor message
void sendMissingMessages(int receiverPort, int originPort, int neededSeqNum);

// helper function to get neighbors: UDP_PORT-1, UDP_PORT+1
std::vector<int> getNeighbors();

// helper function that pick a neighbor by random with the option to exclude specific neighbor
int pickANeighbor(int excludePort);

// helper function for send message through UDP connection
bool sendUDPMessage(int receiverPort, const std::string& message);
```

#### Logic for handleRumorMessage
1. **Parse the received Rumor Message to extract its components**:
   - Split the message by the colon `':'` delimiter to get segments.
   - Assume the format is correct and extract the `senderPort` from the second segment.
   - Extract the content within curly braces `'{}'` and replace commas `','` with colons `':'` to parse the `messageText`, the `originatingPort`, and the `sequenceNumber`.

2. **Check the existence of the originating port in the database**:
   - If no entry exists for the originating port, create a new `DatabaseEntry`.

3. **Insert the message into the database if it's not already present**:
   - Find the sequence number in the `messages` map of the `DatabaseEntry`.
   - If the sequence number is not found, add the message text to the `messages` map.
   - If the sequence number matches the `lowestSeqNum`, increment `lowestSeqNum` and continue to increment while the next sequence number is already in the `messages` map.

4. **After storing the rumor message in the database, respond with a Status Message**:
   - Construct a Status Message that reflects the current state of the database for the originating port.
   - Send the Status Message back to the sender's port to acknowledge receipt of the Rumor Message and to synchronize states if necessary.

#### Logic for handleStatusMessage
1. **Check if the first owner's message is aligned** (vector clock):
    - **case 1**: If the sender's sequence number (`seq`) is equal to mine, proceed to the next step.
    - **case 2**: If the sender's `seq` is less than mine, send a rumor message to the sender for this message.
    - **case 3**: If the sender's `seq` is greater than mine, send my status message with this owner's port as the origin back to the sender to request a rumor.

2. **If the first owner is all aligned**, check for other information in the `statusPairs`:
    - If there is a larger sequence number than what I have in my database or a new port that I don't have, do the following:
      - If the status message for this port has a sequence number of `1`, create an entry for this port in my database.
      - If not, use this port as the origin port and construct a status message to send back to the sender.

3. **If everything is aligned**, decide whether to stop gossiping or pick a new neighbor:
    - Flip a coin to decide the action. If heads, stop gossiping; if tails, pick a new neighbor and continue the gossip protocol.


### statusBoardcastThread
to use for anti-entropy with a timer that when timeout, we send out status message to all neighbors using UDP. 

**Functions involved:**
```cpp!
// main function that has a 10s timer, when timeout, sends out status message to all neighbors
void broadcastStatusPeriodically();

// helper function that perform the actual sending
void broadcastStatusToNeighbors();
```

### Grace Start and Shutdown

**Functions involved:**
```cpp!
P2PServer(int index, int n, int tcpPort);
~P2PServer();

void start();
void shutdownServer();
void waitForThreadsToFinish();
```

We made these helper functions to help safely start and shutdown our server either when receiving the `crash` command or shutdown using `ctrl+c`.


### stopall

The porgam constructs a shell command `pkill -f` that force kill all process named `./process`


## Vulnerability Analysis

### Encryption and Port Configuration
- Messages are transmitted without encryption, leaving the system vulnerable to interceptive attacks like man-in-the-middle.
- The system's architecture hardcodes neighbor ports in the peer-to-peer network, allowing potential infiltration and message interception by malicious entities.

### Network Structure Weaknesses
- Malicious nodes can insert themselves within the network, hoarding messages rather than forwarding them, potentially causing network segmentation and inconsistency.
- Known ports are susceptible to data flooding, putting the network at risk of a distributed denial-of-service (DDoS) attack.

### System Design Assumptions
- System stability is contingent on the assumption that input parameters (`<id>`, `<n>`, `<port>`) are integers, which if violated, results in exceptions.

### Sequence Number Handling
- A preemptive measure is in place where the system disregards the `<messageID>` from `proxy` inputs and instead initializes its own sequence numbering to avoid user input errors.

### Database Access
- Thread safety within the database is ensured by the implementation of mutex locks, mitigating the risk of data corruption due to concurrent access.

### Command Processing
- The system is programmed to acknowledge and process commands that conform to predefined formats and ignores all others until a shutdown command is detected.

### Message Format Assumptions
- The system assumes that all `rumor`, `status`, and messages received from the proxy are in the correct format and only checks the prefix to identify the message type. A specially crafted message that deviates from the expected format could potentially crash the program.

### get chatLog Handle
- When a server doesn't have any message and a `get chatLog` command is sent by the `proxy`, we made the server return `chatLog <empty>` as we found that if we don't put in this special handlation, it will crash `proxy.py`.

## Screenshot of running program.

### How to run program

#### Running Environment

* Please use Ubuntu 22.04.3 LTS.

#### Commands

1. Compile the program. 

We wrote Makefile which can be utilized by the following command: 

```bash
make
```

This command compiles all necessary C++ source files (process.cpp, stopall.cpp) and prepares the server (`./process`) and the shutdown utility (`./stopall`) for execution.

2. Start the Proxy

```bash
python3 proxy.py
python3 prody.py debug # if you want to see all the printout in the .process
```

3. Start the server

You can start the server via input the following command in the terminal interface provided by proxy.py. 

```bash
<id> start <n> <tcpPort>
```

We mainly used: 

```bash
0 start 4 20000
1 start 4 20001
2 start 4 20002
3 start 4 20003
```


4. Interact with the server

As mentioned in the handout, we used the following APIs: 

```bash
# client sends the message to proxy and then proxy transfers it to the server
<id> msg <messagID> <message>

# get the local chat log of that process, each message is separated by comma ","
<id> get chatLog

# call ./stopall to kill all and exit
exit

# the receiver process crashes itself immediately
<id> crash
```

5. The `./stopall` should be executed if the program exit gracefully. But if the prgram does not, we can also manually execute it by run this command.

```bash
./stopall
```


### Program Constraints and Assumptions

- No space and comma are allowed for messageText
- Cannot use port `40000 + index` for proxy communication, these ports are reserved for UDP communication for gossip.
- We defined that the servers are gossiping on the UDP port and their ports are assigned based on `ROOT_ID + index`, hence, if their `index` is consistent which is the `pid` passed from `proxy`, they will have consecutive UDP port and will be neighbors of each other despite the `port` passed from `proxy`.
- Based on the `proxy.py` the program shut down automatically after 120s. 
- The order of get chatLog message is not sorted by timestamp. Based on Slack communication, the TA suggested order doesn't matter.
