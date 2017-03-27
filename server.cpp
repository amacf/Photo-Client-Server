#include <iostream>
#include <sstream>
#include <sys/types.h>	
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>		// For socket polling ioctl()
#include <fstream>			// For file streams
#include "prog3.h"
#define MAXCONNECTIONS 5
#define MAXBUF 201

using namespace std;

// Information stored globally about interaction with this client
int clientSocket, clientID = 0;
stringstream DLLBuffer;			// Buffer of frames of current packet
uint16_t frameExpected = 0;		// Current expected sequence number to be received
int ACKFrameSent = 1;			// How many ACK frames are sent (to determine bad acks)
int currentPhoto = 1;			// Which photo is being sent
long framesReceived = 0, badFrames = 0, duplicateFrames = 0, packetsReceived = 0;
ofstream fdLog;					// File Descriptor for log file


// Simple function to print a message, then exit the program.
void DieWithMessage(char * error){
	cout << error << endl;
	exit(1);
}

// Write the string of length bytes to the current photo file
void writeToFile(char * toWrite, int bytes){
	stringstream fileName;
	fileName << "photonew" << clientID << currentPhoto << ".jpg";	// Form file name
	ofstream file;		
	file.open(fileName.str().c_str(), ios_base::app);				// Open photo file
	file.write(toWrite, bytes);										// Write bytes to file
	file.close();													// Close the photo file
}

// Send a frame on the TCP connection. Simulates physical layer send
void phl_send(string frame){
	int numBytesSent = send(clientSocket, frame.c_str(), frame.length(), 0);
	if (numBytesSent < 0)	//Error checking send()
		DieWithMessage("Call to send() failed");
	else if (numBytesSent != frame.length())	// Check if we have sent all bytes
		DieWithMessage("Call to send() sent unexpected number of bytes");
}

// Send a packet at the data link layer with proper format/
void dll_send(string packet){
	stringstream dataFrame;
	// Format the frame
	dataFrame << (char)0 << (char)0 << (char)FrameType_DataFrame << (char)0 << packet << (char)FrameType_DataFrame;
	phl_send(dataFrame.str());
}

// Terminate socket connection to the client and display connection info
void terminateConnection(){
	cout << "Client " << clientID << " finished after sending " << currentPhoto - 1 << " photos." << endl;
	fdLog << "Finished with: " << endl;
	fdLog << "\t" << currentPhoto - 1 << " photos received" << endl;
	fdLog << "\t" << framesReceived << " good frames received." << endl;
	fdLog << "\t" << badFrames << " bad frames received." << endl;
	fdLog << "\t" << duplicateFrames << " duplicate frames received." << endl;
	fdLog << "\t" << packetsReceived << " packets received." << endl;
	fdLog.close();				// Close the log file
	close(clientSocket);		// Close the client socket
	exit(0);					// Exit the program
}

// Used to check if the client is done sending by waiting 1 second for socket activity
void waitAndClose(){
	// Setup select read buffer
	fd_set bvfdRead;
	int readyNo;
	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	FD_ZERO(&bvfdRead);
	FD_SET(clientSocket, &bvfdRead);

	// Call select to wait for read activity on clientSocket
	readyNo = select(clientSocket+1, &bvfdRead, 0, 0, &timeout);
	if(readyNo < 0){									// Error with select
		printf("Call to select() failed");
		exit(0);
	}
	else if(readyNo == 0)								// Connection timeout
		terminateConnection();
	else {												// Connection still alive
		int bytes_available;
		ioctl(clientSocket, FIONREAD, &bytes_available);// Check if there are bytes
		if(bytes_available==0)
			terminateConnection();
	}
}

// Receive a packet from the DLL on the network layer
void nwl_recv(string packet){
	packetsReceived++;
	// First byte of packet encodes end of photo
	int endPhoto = (int)packet.at(0);
	// Get the photo information from the packet
	char * photoData = (char*) malloc(packet.length() - 1);
	memcpy((void*)photoData,(void*)(packet.c_str() + 1), packet.length()-1);
	// Write the photodata to file
	writeToFile(photoData, packet.length()-1);

	// If this is the last packet of the photo, send a NWL ACK
	if (endPhoto == 1){
		dll_send("ACK");
		currentPhoto += 1;
		// Check to see if client is done closing
		waitAndClose();
	}
}

// Send a DLL ACK
void dll_sendAck(int seq_num){
	// Form the correct ACK Frame
	stringstream ackFrame;
	uint16_t seq = seq_num;
	ackFrame.write((char*)&seq,2);
	ackFrame << (char)FrameType_ACKFrame << (char)0;
	ackFrame.write((char*)&seq,2);

	// If this is the 13th ACK frame sent, form a false ACK
	if(ACKFrameSent == 13){
		// Flip last byte to simulate errors
		int size = ackFrame.str().length();
		char * newString = (char*) ackFrame.str().c_str();
		newString[size-1] = ~(newString[size-1]);
		ackFrame.str(""); ackFrame.write(newString,size);
		ACKFrameSent = 0;
		fdLog << "Sent bad ACK for frame " << seq_num << "." << endl;
	} else{
		// Otherwise form a good ACK
		ACKFrameSent++;
		fdLog << "Sent good ACK for frame " << seq_num << "." << endl;
	}
	// Send the ACK frame to the client
	phl_send(ackFrame.str());
}

// Receive a DLL frame from the physical layer
void dll_recv(string frame){
	// Parse the frame for all info
	uint16_t sequenceNumber = *((uint16_t*)frame.c_str());
	FrameType type = (FrameType)((char)(frame.at(2)) - '0');
	int EOP = (int)((char) frame.at(3) - '0');
	uint16_t ed = *((uint16_t*)(frame.c_str() + frame.length() - 2));
	char * dataField = (char*) malloc(frame.length()-6);

	// Copy the payload from the frame
	memcpy((void*)dataField,(void*)(frame.c_str() + 4), frame.length()-6);
	string data = string(dataField);

	// readEd is the ED calculated from the frame
	uint16_t readEd = 0;
	uint16_t curTwoBytes;	// Current two bytes used for 2-byte XOR folding
	// Calculate readEd
	for (int i = 0; i < frame.length() - 3; i+=2){
		memcpy((void*)&curTwoBytes, (void*)(frame.c_str() + i), 2);
		readEd = readEd ^ curTwoBytes;
	}

	// If what we received is different from calculated, dont send ACK
	if (readEd != ed){
		badFrames++;
		fdLog << "Received bad frame " << sequenceNumber << "." << endl;
		// Bad frame, do not send acknowledgement
		return;
	}
	else if (sequenceNumber != frameExpected){
		// Received Duplicate, resend last ACK
		fdLog << "Received duplicate frame " << sequenceNumber << "." << endl;
		dll_sendAck(sequenceNumber);
		duplicateFrames++;
		return;
	}else{	// Got a good frame, send an ACK
		fdLog << "Received good frame " << sequenceNumber << "." << endl;
		// Write dataField into the current packet buffer, DLLBuffer
		DLLBuffer.write(dataField, frame.length()-6);
		dll_sendAck(sequenceNumber);
		frameExpected++;
		framesReceived++;
	}

	// If we have the whole packet, send it to the network layer
	if(EOP == 1){
		nwl_recv(DLLBuffer.str());
		DLLBuffer.str("");
	}
}

// Handle this one clientSocket. Every instance of this function is on a seperate process
void handleTCPClient(){ 
	// Wait to recv full frame before sending to DL Layer
	char buf[MAXBUF];
	// Put frame bytes into byteStream until we have the whole thing
	stringstream byteStream;
	int messageLength = 0;
	int bytesOfMessage = 0;
	// Receive the 1-byte ID of user
	while(clientID==0){
		char ID;
		int numBytesReceived = recv(clientSocket, &ID, 1, 0);
		if(numBytesReceived < 0)	// Error check recv()
			DieWithMessage("Call to recv() failed");
		if(numBytesReceived > 0){
			clientID = (int)ID;
		}
	}

	// Open the logfile with correct name
	char * logFile = (char*)malloc(13);
	sprintf(logFile,"server_%1d.log",clientID);
	fdLog.open(logFile);
	fdLog << "Connected to client " << clientID << endl; 

	// numTries is how many times we have received zero bytes from the client
	int numTries = 0;
	while (1){
		// Receive more bytes
		int numBytesReceived = recv(clientSocket, buf, MAXBUF - 1, 0);
		buf[numBytesReceived] = '\0';
		if(numBytesReceived < 0)	// Error check recv()
			DieWithMessage("Call to recv() failed");
		if(numTries==25)
			// If recv got twenty-five consecutive zero-byte reads, try to close connection
			waitAndClose();
		if(numBytesReceived > 0){
			numTries = 0;
			// We have received something
			if(messageLength == 0)
				messageLength = (uint8_t)buf[0];
			// Put all received bytes in the bytestream
			bytesOfMessage += numBytesReceived;
			// If we have all of the bytes of this message...
			if(bytesOfMessage >= messageLength + 1){
				// Calculate any bytes that arent from this message
				int numNonMessageBytesInBuf = bytesOfMessage - (messageLength + 1);
				// Write buffered bytes of this message to the frame
				byteStream.write(buf, numBytesReceived - numNonMessageBytesInBuf);\
				stringstream toDLL;
				toDLL.write(&byteStream.str().c_str()[1], byteStream.str().length() - 1);
				dll_recv(toDLL.str());

				// Clear frame info
				byteStream.str("");
				messageLength = 0;
				bytesOfMessage = 0;
				// Write any remaining bytes in buffer to front of buffer
				if(numNonMessageBytesInBuf > 0){
					byteStream.write(&buf[numBytesReceived-numNonMessageBytesInBuf],numNonMessageBytesInBuf);
					bytesOfMessage += numNonMessageBytesInBuf;
					messageLength = (uint8_t)byteStream.str().at(0);
				}
			}else{	// We dont have whole frame yet, keep receiving
				bytesOfMessage += numBytesReceived;
				byteStream.write(buf, numBytesReceived);
			}
		} else numTries ++;
	}
	exit(0);
}

// Main for photoServer
int main(int argc, char * argv[]){
	
	// Create a socket for reading info from clients
	int serverSocket;
	if ((serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
		DieWithMessage("Call to socket() failed");
	// Setup socket to receive connections immediately, and drop all after termination
	int optVal = 1;
	socklen_t optLen = sizeof(optVal);
	setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (void*) &optVal, optLen);

	// Build the local address structure
	struct sockaddr_in serverAddress;					
	memset(&serverAddress, 0, sizeof(serverAddress));	// zero out structure
	serverAddress.sin_family = AF_INET;					// Internet address family (TCP/IP)
	serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);	// Any incoming interface
	serverAddress.sin_port = htons(WELLKNOWNPORT);		// Local port

	// Bind to the local address port
	if ((bind(serverSocket,(sockaddr *) &serverAddress, sizeof(serverAddress)) < 0))
		DieWithMessage("Call to bind() failed");
		
	// Listen for all traffic on this port
	if (listen(serverSocket, MAXCONNECTIONS) < 0)
		DieWithMessage("Call to listen() failed");

	// Create a structure for storing client information
	struct sockaddr_in clientAddress;
	// Set length of client address structure
	socklen_t clientAddressLength = sizeof(clientAddress);


	while(1){
		cout << "Waiting for clients" << endl;
		// Block until a client connects, store client info in clientAddress
		clientSocket = accept(serverSocket,(sockaddr *) &clientAddress, &clientAddressLength);
		if (clientSocket < 0)
			DieWithMessage("Call to accept() failed");
		// clientSocket is now connected to a client.
		char *clientName;
		// Get the client address and print it and the port.
		if ((clientName = inet_ntoa(clientAddress.sin_addr)) != NULL)
			cout << "Handling client " << clientName << "/" << ntohs(clientAddress.sin_port) << endl;
		else
			cout << "Unable to get client address" << endl;
		
		int retval;
		if ((retval=fork()) == 0){
			close(serverSocket);
			handleTCPClient(); // Handle this one client, then terminate.
			close(clientSocket);
			exit(0);
		}
		else if (retval < 0)
			DieWithMessage("Call to fork() failed");

	}
	close(serverSocket);
}
