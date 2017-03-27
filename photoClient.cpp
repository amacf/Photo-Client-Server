// Andrew McAfee
// CS3516 Prog3: Photo Client

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sstream>
#include <iostream>
#include <fstream>
#include <time.h>
#include "prog3.h"
#define MAXBUF 200

using namespace std;

int hostname_to_ip(char *, char *);
void connectSocket(char *);
void dll_recvAck();

// Information stored globally about interaction with the server
int sock;
int totalFrames, totalRetry, totalGoodACK, totalBadACK = 0;
char * lastFrame = (char*) malloc(MAXBUF + 10);		// Last frame sent
int lastFrameSize, frameNum = 0;					// Size of last send, frames sent
uint16_t seqNum = 0;								// Current frame sequence number
ofstream fdLog;										// File descriptor of log


// Simple function to print a message, then exit the program.
void DieWithMessage(char * error){
	printf("%s\n",error);
	exit(1);
}

// Send a packet of length byteNum at the physical layer.
void phl_send(char * packet, int byteNum){
	// Copy frame being sent into last sent frame
	memcpy((void*)lastFrame,(void*)packet,byteNum);
	lastFrameSize = byteNum;

	// If this is the fourth frame being sent...
	if(frameNum == 4){
		// Form a bad frame
		packet[byteNum-1] = ~(packet[byteNum-1]);
		fdLog << "Sent bad frame " << seqNum << endl;
		frameNum = 0;
	} else{	// Other wise form a good frame.
		fdLog << "Sent good frame " << seqNum << endl;
		frameNum ++;
		totalFrames++;
	}

	// Send the frame to the server
	int sendBytes = send(sock, packet, byteNum, 0);
	if(sendBytes < 0){	// Check to make sure it sent the whole command
		printf("Call to send() failed");
		exit(0);
	}
	else if (sendBytes != byteNum){	// Error check
		printf("Call to send() sent unexpected number of bytes");
		exit(0);
	}
}

// Receive an ACK at the physical layer
void phl_recvAck(){
	char * buf = (char*) malloc(7);
	// Receive bytes
	int numBytesReceived = recv(sock, buf, 6, 0);
	buf[numBytesReceived] = '\0';
	if(numBytesReceived < 0)	// Error check recv()
		DieWithMessage("Call to recv() failed");
	if(numBytesReceived > 0){ // We received some bytes
		// Get the sequence number and error detection num from the ACK frame
		uint16_t sequenceNumber = *((uint16_t*)buf);
		uint16_t ed = *((uint16_t*)(&(buf[4])));

		// Check if this is a good ACK, resend if it is bad
		if(sequenceNumber != ed){
			fdLog << "Received bad ACK for frame " << sequenceNumber << endl;
			phl_send(lastFrame, lastFrameSize);
			totalRetry++;
			dll_recvAck(); // Receive an ACK at the datalink layer
			totalBadACK++;
		} else {
			fdLog << "Received good ACK for frame " << sequenceNumber << endl;
			totalGoodACK++;
		}
	}
}

// Receive an ACK at the DLL
void dll_recvAck(){
	// Setup select read buffer
	fd_set bvfdRead;
	int readyNo;
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	FD_ZERO(&bvfdRead);
	FD_SET(sock, &bvfdRead);

	// Call select to wait for read activity on sock
	readyNo = select(sock+1, &bvfdRead, 0, 0, &timeout);
	if(readyNo < 0){		// Select error
		printf("Call to select() failed");
		exit(0);
	}
	else if(readyNo == 0){	// Select caused timeout
		phl_send(lastFrame, lastFrameSize);
		dll_recvAck();
	}
	else {					// Select sensed socket read bytes
		phl_recvAck();
	} 
}

// Receive a network layer ACK for a photo
void nwl_recvAck(){
	char * buf = (char*) malloc(9);
	// Receive bytes
	int numBytesReceived = recv(sock, buf, 8, 0);
	buf[numBytesReceived] = '\0';
	if(numBytesReceived < 0)	// Error check recv()
		DieWithMessage("Call to recv() failed");
	if(numBytesReceived > 0){
		// If we got bytes, check if it was a dataFrame ACK.
		FrameType type = (FrameType)((char)buf[2]);
		char * msg = (char*)malloc(4);
		msg[3] = '\0';
		memcpy((void*)msg,(void*)(&(buf[4])),3);
		if (strcmp(msg, "ACK")!=0 || type != FrameType_DataFrame){
			DieWithMessage("Failed to recieve NWL ACK");
		}
	}
}

// Send a packet at the data link layer
void dll_send(char * packet, int numBytes){
	int packetPosition = 0;
	int payloadSize = numBytes;	// Total bytes of this packet
	while(payloadSize != 0){
		// EOP indicates end of packet
		char EOP;
		// Setup the payload size of this frame (max is 124 bytes)
		int curLoadSize;
		if (payloadSize < 124){
			curLoadSize = payloadSize;
			EOP = 1 + '0';
		} else { 
			curLoadSize = 124;
			EOP = 0 + '0';
		}
		payloadSize -= curLoadSize;

		// Form the correct frame headers
		char ft = FrameType_DataFrame + '0';//
		uint16_t ed = 0;
		stringstream frame;
		frame.write((char*)&seqNum,2);
		frame << (char)ft << (char)EOP;

		// Write the payload into the frame
		frame.write(packet + packetPosition, curLoadSize);

		// Calculate the ED for this data frame
		uint16_t curTwoBytes;
		for (int i = 0; i < curLoadSize + 3; i+=2){
			memcpy((void*)&curTwoBytes, (void*)(frame.str().c_str() + i),2);
			ed = ed ^ curTwoBytes;
		}

		// Write the ED onto the dataframe
		frame.write((char*)&ed, 2);

		// Attach the header byte to the frame (indicates number of bytes of frame)
		stringstream wholeFrame;
		uint8_t size8 = frame.str().length();
		wholeFrame.write((char*)&size8,1);
		wholeFrame << frame.str();

		// Send the frame, then receive an ACK for the frame.
		phl_send((char*)wholeFrame.str().c_str(), curLoadSize + 7);
		dll_recvAck();
		packetPosition += curLoadSize;
		seqNum++;
	}
}

// Main for photoClient
int main(int argc, char **argv){
	
	// Get the IP from hostname
	char *hostname;
	char ip[100];
	hostname = argv[1];
	hostname_to_ip(hostname , ip);

	// Connect to the ip
	connectSocket(ip);

	// Setup the clientID
	int clientID;
	char *clientIDstring;
	clientIDstring = argv[2];
	clientID = atoi(clientIDstring);

	// Open the log file with proper name
	char *fileName = (char*)malloc(15);
	sprintf(fileName, "client_%1d.log",clientID);
	fdLog.open(fileName);
	fdLog << "Connected to host " << hostname << endl;

	//Send the clientID to the server
	phl_send((char*)&clientID,1);

	int numPhotos, j = 1;
	string photoName;
	char *photoData;
	numPhotos = atoi(argv[3]);

	struct timeval start, end;	// Store when program started sending
	gettimeofday(&start, NULL); // measure total execution time

	//Start reading photos
	while (j <= numPhotos){
		//name of current photoij.jpg where 'i' is clientID and 'j' is number of photo
		stringstream fileName;
		fileName << "photo" << clientID << j << ".jpg";
		ifstream imgFile;
		// Open the photo
		imgFile.open(fileName.str().c_str(), ios::binary);
		if(!imgFile.is_open()){
			DieWithMessage("Could not open file");
		}

		cout << "Sending image " << j << "..." << endl;
		fdLog << "Sending image " << j << "..." << endl;

		// Read bytes from file and format as packet
		unsigned char buffer[201];
		int i;
		unsigned int totalRead = 0;
		while(!imgFile.eof()){
			imgFile.read((char*)&buffer[1], 200);

			// Insert packet-heading end-of-frame byte
			buffer[0] = imgFile.eof() ? (char)1 : (char)0;
			// Send the frame to the server
			dll_send((char*)buffer, imgFile.gcount() + 1);
			totalRead += imgFile.gcount();
		}
		nwl_recvAck();	// Sent a photo, send a NWL ACK
		j++;
	}

	// Get end of sent time and do calculation
	gettimeofday(&end, NULL);
	unsigned long executionTime = ((end.tv_sec * 1000000 + end.tv_usec)
			- (start.tv_sec * 1000000 + start.tv_usec))/1000;

	// Print data to the log.
	fdLog << "Total frames sent: " << totalFrames << endl;
	fdLog << "Total retransmissions: " << totalRetry << endl;
	fdLog << "Total good ACK's: " << totalGoodACK << endl;
	fdLog << "Total bad ACK's: " << totalBadACK << endl;
	fdLog << "Total execution time (ms): " << executionTime << endl;

}

// Convert a hostname to ip using gethostbyname() DNS interface
int hostname_to_ip(char * hostname , char* ip){
	struct hostent *he;
	struct in_addr **addr_list;
	int i;

	// Get the IP from the hostname
	if ( (he = gethostbyname( hostname ) ) == NULL)
	{
		puts("hostname error");
		return 1;
	}

	// Get the correct address from the address list
	addr_list = (struct in_addr **) he->h_addr_list;
	for(i = 0; addr_list[i] != NULL; i++)
	{
		strcpy(ip , inet_ntoa(*addr_list[i]) );
		return 0;
	}

	return 1;
}

// Connect to the server indicated address ip on port WELLKNOWNPORT
void connectSocket(char * ip){
	//Create socket
	sock = socket(AF_INET , SOCK_STREAM , 0);
	if(sock == -1){
		printf("could not create socket");
	}

	// Build server address
	struct sockaddr_in server;
	server.sin_addr.s_addr = inet_addr(ip);
	server.sin_family = AF_INET;
	server.sin_port = htons(WELLKNOWNPORT);

	// connect to server
	if (connect(sock , (struct sockaddr *)&server , sizeof(server)) < 0){
		puts("connect failed");
		return;
	}
	return;
}


