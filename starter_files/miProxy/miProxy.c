#include <arpa/inet.h> //close
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> //strlen
#include <sys/socket.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO, FD_SETSIZE macros
#include <sys/types.h>
#include <unistd.h> //close
#include <netdb.h>

#define MAXCLIENTS 30
#define BUF_SIZE 2048
#define MAX_BITRATE_NUM 64
#define MAX_CHUNKNAME_LEN 256
#define MAX_SERVER_LINE 100

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))

int get_listen_socket(struct sockaddr_in *address, int port)
{
    int yes = 1;
    int server_socket;
    // create a master socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket <= 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // set master socket to allow multiple connections ,
    // this is just a good habit, it will work without this
    int success =
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (success < 0)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // type of socket created
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = INADDR_ANY;
    address->sin_port = htons((u_short)port);

    // bind the socket to localhost port 8888
    success = bind(server_socket, (struct sockaddr *)address, sizeof(*address));
    if (success < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    printf("---Listening on port %d---\n", port);

    // try to specify maximum of 3 pending connections for the server socket
    if (listen(server_socket, 3) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    return server_socket;
}

void extract_bitrate_list(char* manifest_buffer, int bitrates[][MAX_BITRATE_NUM], int* bitrate_num, int i) {
    printf("extract\n");
    const char* key = "bitrate=";
    char* ret = manifest_buffer;
    while(1) {
        char rate[20];
        if((ret = strstr(ret, key)) == NULL) break;
        else {
            ret += 9;
            int count = 0;
            while(ret[count] != '"') {
                rate[count] = ret[count];
                count++; 
            }
            rate[count] = '\0';
            bitrates[i][bitrate_num[i]] = atoi(rate);
            bitrate_num[i]++;
        }
    }
}

void modify_to_nolist(char* buffer, char* nolist_buffer) {
    char* start_ptr = strstr(buffer, "big_buck_bunny");
    char* end_ptr = strstr(buffer, ".f4m");

    size_t prefix_len = start_ptr - buffer;
    size_t suffix_len = strlen(end_ptr + 4);

    strncpy(nolist_buffer, buffer, prefix_len);
    nolist_buffer[prefix_len] = '\0';


    strcat(nolist_buffer, "big_buck_bunny_nolist.f4m");


    strcat(nolist_buffer, end_ptr + 4);
}

int main(int argc, char *argv[])
{
    int use_dns = 0;

    int send_idx = 0, recv_idx = 0; // for debugging only, you can ignore it

    int serverPort = 80; // the port for server, 80 as the default HTTP port

    int listen_port;
    char *server_file;
    char *log_file;
    double alpha;

    // for --nodns load balance
    unsigned long server_addrs[MAX_SERVER_LINE]; // sockaddr_in.in_addr.s_addr
    int server_num = 0;
    int request_cnt = 0;

    // process the args
    if (argc == 6)
    {
        // used in mandatory part
        // no-dns case
        use_dns = 0;
        // listen port for connection
        listen_port = atoi(argv[2]);
        // server ip
        server_file = argv[3];
        // value for <alpha>
        alpha = atof(argv[4]);
        // log file path
        log_file = argv[5];

        // =========== extract server ips from txt file ==========
        FILE *fp = fopen(server_file, "r");
        if (fp == NULL)
        {
            perror("open server file error");
            exit(1);
        }
        int file_read_len;
        size_t line_len = 64;
        char *line_buf = NULL; // (char *)malloc(100 * sizeof(char));
        while ((file_read_len = getline(&line_buf, &line_len, fp)) != -1)
        {
            if (line_buf[file_read_len - 1] == '\n')
            {
                line_buf[file_read_len - 1] = '\0';
            }
            printf("server ip:[%s] cnt: %d\n", line_buf, server_num + 1);
            server_addrs[server_num] = ntohl(inet_addr(line_buf));
            server_num++;
        }
        fclose(fp);
        if (server_num <= 0)
        {
            printf("%s is empty", server_file);
            exit(1);
        }
        // ============= end of extract server ips ===============

        FILE *fp_log = fopen(log_file, "w"); // clear up the log file
        if (fp_log == NULL)
        {
            perror("open log file");
            exit(1);
        }
        fclose(fp_log);
    }
    else if (argc == 7)
    {
        // reserved for bonus part
    }
    else
    {
        // exit for other args cases
        fprintf(stderr, "Argc as %d wrong\n", argc);
        exit(0);
    }



    if(alpha<0 || alpha>1) {
        perror("alpha is out of range");
        exit(-1);
    }

    int proxy_listen_socket, addrlen, activity, valread;
    // define the para for each server-client pair
    int client_sockets[MAXCLIENTS] = {0}; // client socket for each streaming
    int server_sockets[MAXCLIENTS] = {0}; // server sockets for each streaming

    int bitrates[MAXCLIENTS][MAX_BITRATE_NUM]; // bitrate values for the parsed bitrates from f4m file
    int bitrate_num[MAXCLIENTS] = {0};         // bitrate number for the paresed bitrates from f4m file

    int is_chunk[MAXCLIENTS];                       // is_chunk flag for indicating whether chunk is tranferred in the streaming
    double T_curN[MAXCLIENTS];                      // the current throughtput
    char chunknames[MAXCLIENTS][MAX_CHUNKNAME_LEN]; // the chunk name for current streaming (used in the log)
    int cur_bitrate[MAXCLIENTS] = {0};              // the current bitrate for the video streaming
    int chunk_length[MAXCLIENTS] = {0};             // the length for this chunk (include the HTTP header)

    struct timeval chunk_start_time[MAXCLIENTS];   // the start time for each chunk
    int chunk_sent_length[MAXCLIENTS] = {0};       // the sent length for current chunk (include the HTTP header)
    int chunk_http_header_recvd[MAXCLIENTS] = {0}; // the http header for the chunk is recvd or not
    struct in_addr server_ips[MAXCLIENTS];

    // initialize the arrays
    for (int _i = 0; _i < MAXCLIENTS; _i++)
    {
        T_curN[_i] = 0.0;
        for (int _j = 0; _j < MAX_BITRATE_NUM; _j++)
        {
            bitrates[_i][_j] = 0;
        }
        memset(chunknames[_i], 0, MAX_CHUNKNAME_LEN);
    }
    // end of initialization

    struct sockaddr_in address;
    proxy_listen_socket = get_listen_socket(&address, listen_port);
    char buffer[BUF_SIZE];

    // accept the incoming connection
    addrlen = sizeof(address);
    puts("Waiting for connections ...");
    // set of socket descriptors
    fd_set readfds;

    FILE *file = fopen(server_file, "r"); 
    if (file == NULL) {
        perror("fopen");
        exit(1);
    }

    char ip_addresses[MAX_SERVER_LINE][100]; 
    for (int i = 0; i < server_num; i++) {
        if (fgets(ip_addresses[i], 100, file) == NULL) {
            perror("fgets");
            fclose(file);
            exit(1);
        }
        // 去除末尾换行符
        size_t ip_len = strlen(ip_addresses[i]);
        if (ip_addresses[i][ip_len - 1] == '\n') {
            ip_addresses[i][ip_len - 1] = '\0';
        }
    }

    fclose(file);


    while (1)
    {
        // clear the socket set
        FD_ZERO(&readfds);

        // add listen socket to set
        FD_SET(proxy_listen_socket, &readfds);
        // add all the client sockets to the set
        for (int i = 0; i < MAXCLIENTS; i++)
        {
            int client_sock = client_sockets[i];
            if (client_sock != 0)
            {
                FD_SET(client_sock, &readfds);
            }
        }
        // add all the server sockets to the set
        for (int i = 0; i < MAXCLIENTS; i++)
        {
            int server_sock = server_sockets[i];
            if (server_sock != 0)
            {
                FD_SET(server_sock, &readfds);
            }
        }

        // wait for an activity on one of the sockets , timeout is NULL ,
        // so wait indefinitely
        activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR))
        {
            perror("select error");
        }

        // ====================================================
        // ==============START OF listen socket================
        // If something happened on the listen socket ,
        // then its an incoming connection, call accept()
        if (FD_ISSET(proxy_listen_socket, &readfds))
        {
            // TODO: write your code here
            int browser_listen_socket = accept(proxy_listen_socket, (struct sockaddr *)&address, (socklen_t *)&addrlen);
            if (browser_listen_socket == -1) {
                perror("accept");
                //may delete next line
                exit(EXIT_FAILURE);
            }

             // inform user of socket number - used in send and receive commands
            printf("\n---New connection---\n");
            printf("socket fd is %d , ip is : %s , port : %d \n", browser_listen_socket,
                    inet_ntoa(address.sin_addr), ntohs(address.sin_port));
    
            // Indicate the available position of client sockets (available position of server sockets is the same)(Index of webserver addresss should be the same)
            int cli_socket_index = -1;

            // add new socket to the array of sockets
            for (int i = 0; i < MAXCLIENTS; i++) {
                if(client_sockets[i] == 0) {
                    //find available position, and mark as cli_socket_index
                    cli_socket_index = i;
                    break;
                }
            }

            //if there's no available position
            if (cli_socket_index == -1) {
                perror("Exceeding the maximum number of clients");
                close(browser_listen_socket);
            }
            else {
                //Create a new connection to web server

                int server_addr_index = cli_socket_index % server_num; //Round-Robin Load Balancer

                int server_socket;
                struct addrinfo hints, *servinfo, *p;
                int rv;

                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;

                if((rv = getaddrinfo(ip_addresses[server_addr_index], "80", &hints, &servinfo)) != 0) {
                    perror("getaddrinfo");
                    exit(1);
                }

                for(p = servinfo; p != NULL; p = p->ai_next){
                    if((server_socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
                        perror("client: socket");
                        continue;
                    }

                    if(connect(server_socket, p->ai_addr, p->ai_addrlen) == -1) {
                        close(server_socket);
                        perror("clinet: connect");
                        continue;
                    }
                    break;
                }

                if (p == NULL) {
                    perror("clinet: failed to connect");
                    return -1;
                }

                inet_pton(AF_INET, ip_addresses[server_addr_index], &server_ips[cli_socket_index].s_addr);

                freeaddrinfo(servinfo);

                client_sockets[cli_socket_index] = browser_listen_socket;
                server_sockets[cli_socket_index] = server_socket;
            }
           
        }
        // ===================END OF listen socket========================
        // ===============================================================

        // ===============================================================
        // ===================START OF client socket======================
        // "for recving HTTP GET request from client only"
        // 1. Intercept HTTP GET requests from client
        // 2. Handle f4m data GET request
        // 3. Handle chunk data GET request
        // 4. Handle other request
        for (int i = 0; i < MAXCLIENTS; i++) {
            // TODO: write your code here
            if (FD_ISSET(client_sockets[i], &readfds)) {
		printf("client start\n");
                if((valread = recv(client_sockets[i], buffer, BUF_SIZE - 1, 0)) <= 0) {
                    // got error or connection closed by client
                    if (valread == 0) {
                        // connection closed
                        close(client_sockets[i]);
                        client_sockets[i] = 0;
                        close(server_sockets[i]);
                        server_sockets[i] = 0;
                    }
                    else {
                        perror("recv");
                    }
                    continue;
                }
                else {
                    // got some data from client
                    buffer[valread] = '\0';

                    if(strstr(buffer, "big_buck_bunny.f4m")) {
                        //the client requests for the video manifest
			printf("client: video\n");
                        if(bitrate_num[i] == 0) {
                            // No bitrate list yet
                            if(send(server_sockets[i], buffer, valread, 0) < 0) {
                                // sent the original .f4m request to the server
                                perror("Error sending client's data to the web server");
                            }

                            // Receive the original .f4m file with bitrate list, keep it and don't send back to the clients
                            char manifest_buffer[BUF_SIZE];
                            int manifest_valread = 0;
                            int total_manifest_valread = 0;

                            if((manifest_valread = recv(server_sockets[i], manifest_buffer, BUF_SIZE-1, 0)) < 0) {
                                perror("Error receive the manifest data from server");
                            }
                            else if (manifest_valread == 0) {
                                // connection is closed by the web server 
                                close(client_sockets[i]);
                                client_sockets[i] = 0;
                                close(server_sockets[i]);
                                server_sockets[i] = 0;
                            }
                            else {
                                // got the original .f4m response from the server
                                total_manifest_valread += manifest_valread;
                                manifest_buffer[manifest_valread] = '\0';

                                int headerLength;
                                int contentLength;

                                if(strstr(manifest_buffer, "\r\n\r\n")!= NULL) {
                                    char * endOfHeader = strstr(manifest_buffer, "\r\n\r\n");
                                    headerLength = 4 + endOfHeader - manifest_buffer;
                                }
                                else{
                                    perror("Error: HTTP header is not found");
                                    exit(EXIT_FAILURE);
                                }

                                //the size of the HTTP body in bytes
                                if(strstr(manifest_buffer, "Content-Length: ") != NULL) {
                                    char * contentStart=strstr(manifest_buffer, "Content-Length: ")+16;
                                    char * contentEnd=strstr(contentStart, "\r\n"); 
                                    char digit[contentEnd-contentStart+1];
                                    contentLength=atoi(strncpy(digit, contentStart, contentEnd-contentStart));
                                }
                                else {
                                    perror("Error: Content-Length is not found");
                                    exit(EXIT_FAILURE);
                                }

                                int http_packet_len = headerLength + contentLength;

                                while(total_manifest_valread < http_packet_len) {
                                    //!!!!!!!!!!!!!!!!这里buffer很可能超掉，size要再改改！！
                                    if((manifest_valread = recv(server_sockets[i], manifest_buffer, BUF_SIZE-1, 0)) < 0){
                                        perror("Error receiving web server's data");
                                    }
                                    else if (manifest_valread == 0) {
                                        // connection is closed by the web server 
                                        close(client_sockets[i]);
                                        client_sockets[i] = 0;
                                        close(server_sockets[i]);
                                        server_sockets[i] = 0;
                                        break;
                                    }
                                    else {
                                        total_manifest_valread += manifest_valread;
                                    }
                                }

                                if (manifest_valread == 0) continue;
                                
                                extract_bitrate_list(manifest_buffer, bitrates, bitrate_num, i);
                            }
    
                        }

                        // There is bitrate list
                        // Modify the original .f4m request to _nolist.f4m
                        char nolist_buffer[BUF_SIZE]; // store the _nolist.f4m http request

                        modify_to_nolist(buffer, nolist_buffer);
                        valread = strlen(nolist_buffer);
                        if(send(server_sockets[i], nolist_buffer, valread, 0) < 0) {
                            // sent the  _nolist.f4m request to the server
                            perror("Error sending client's data to the web server");
                        }
                    }
                    else if(strstr(buffer,"Seg")){
                        //select bitrate
			printf("client: seg\n");
                        int select_bitrate = 0;
                        for(int j = 0; j < MAX_BITRATE_NUM; j++) {
                            if(T_curN[i] >= 1.5 * bitrates[i][j]) {
                                select_bitrate = bitrates[i][j];
                            }
                        }
                        //modify the original chunk request to the selected bitrate
                        //in form of path/to/video/<bitrate>Seg<num>-Frag<num>
                        char *start_ptr = strstr(buffer, "Seg");
                        char *buffer_end=strstr(buffer,"Seg");
                        //find the end of the buffer
                        while(buffer[buffer_end-buffer]!=' '){
                            buffer_end++;
                        }
                        buffer_end--;
                        //find the laset '/'
                        char *end_ptr = strrchr(start_ptr, '/');
                        //bit rate is between the last '/' and 'Seg', modify the original bitrate to selected one
                        char new_buffer_first[BUF_SIZE];
                        char new_buffer_second[BUF_SIZE];
                        strncpy(new_buffer_first, buffer, end_ptr-buffer+1);
                        //copy the string after seg
                        strncpy(new_buffer_second, start_ptr, buffer_end-start_ptr);
                        //change select_bitrate to string
                        char bitrate_str[10];
                        sprintf(bitrate_str, "%d", select_bitrate);
                        strcat(new_buffer_first, bitrate_str);
                        strcat(new_buffer_first, new_buffer_second);
                        //copy the new buffer to buffer
                        strncpy(buffer, new_buffer_first, strlen(new_buffer_first));
                        //chunkname
                        char * chunk_name = (char *) malloc(sizeof(char) * 100);
                        strncpy(chunk_name, start_ptr, buffer_end-start_ptr);
                        strcat(chunk_name,"\0");
                        strcpy(chunknames[i],chunk_name);

                        valread=strlen(buffer);
                        if(send(server_sockets[i], buffer, valread, 0) < 0) {
                            perror("Error sending client's data to the web server");
                        }
                    }
                    else {
                        //Handle other requests from clients
			printf("client: other requests\n");
                        if(send(server_sockets[i], buffer, valread, 0) < 0) {
                            perror("Error sending client's data to the web server");
                        }
                    }
                    
                }
            }
        }
        // ==================END OF client socket=======================
        // =============================================================

        // =============================================================
        // ==================START OF server socket=====================
        // 1. Handle server disconnection
        // 2. Intercept HTTP reply from server
        // 3. Analyze the reply and calculate the bitrate
        // 4. Send the HTTP reply to the corresponding client
        // 5. Write out the log
        for (int i = 0; i < MAXCLIENTS; i++)
        {
            // TODO: write your code here
            if (FD_ISSET(server_sockets[i], &readfds)&&server_sockets[i]!=0){
		printf("server start\n");
                //handle server disconnection
                if((valread = recv(server_sockets[i], buffer, BUF_SIZE - 1, 0)) <= 0) {
                    // got error or connection closed by server
                    if (valread == 0) {
                        // connection closed
                        close(client_sockets[i]);
                        client_sockets[i] = 0;
                        close(server_sockets[i]);
                        server_sockets[i] = 0;
                    }
                    else {
                        perror("recv");
                    }
                    continue;
                }
                else {
                    // got some data from server
                    buffer[valread] = '\0';

                    //send to client
                    if(send(client_sockets[i], buffer, valread, 0) < 0) {
                        perror("Error sending server's data to the client");
                    }


                    if(!is_chunk[i]) {
                        //the chunk is not transferred yet
                        is_chunk[i] = 1;
                        //start timing
                        gettimeofday(&chunk_start_time[i], NULL);
                    }

                    int headerLength;
                    int contentLength;
                    //the end of an HTTP header
                    if(strstr(buffer,"\r\n\r\n")!=NULL){
                        char * endOfHeader=strstr(buffer,"\r\n\r\n");
                        //“\r\n\r\n” (i.e., strlen(”\r\n\r\n"))=>4 adds the end position of the header, and finally subtracts buffer (the start position of the header). This gives the total length of the header.
                        headerLength=4+endOfHeader-buffer;
                    }
                    else{
                        perror("Error: HTTP header is not found");
                        exit(EXIT_FAILURE);
                    }

                    //the size of the HTTP body in bytes
                    if(strstr(buffer, "Content-Length: ")!=NULL) {
                        //len (Content-Length: )=16
                        char * contentStart=strstr(buffer, "Content-Length: ")+16;
                        char * contentEnd=strstr(contentStart, "\r\n");
                        //extra 1 byte for the null terminator
                        char digit[contentEnd-contentStart+1];
                        contentLength=atoi(strncpy(digit, contentStart, contentEnd-contentStart));
                    }
                    else
                    {
                        perror("Error: Content-Length is not found");
                        exit(EXIT_FAILURE);
                    }

                    

                    int remainingLength=contentLength+headerLength-valread;
                    char complete_buffer[valread+remainingLength+1];
                    strcpy(complete_buffer, buffer);
		    printf("reamining length: %d\n",remainingLength);
                    //receive remaining data
                    while(remainingLength>0){
                        memset(buffer, 0, BUF_SIZE);
                        int recvLength=recv(server_sockets[i], buffer, MIN(remainingLength, BUF_SIZE-1), 0);
                        if(recvLength<0){
                            perror("Error: receive remaining data");
                            exit(EXIT_FAILURE);
                        }
                        else if(recvLength==0){
                            perror("Error: connection closed by server");
                            exit(EXIT_FAILURE);
                        }
                        else{
                            send(client_sockets[i], buffer, recvLength, 0);
                            remainingLength-=recvLength;
                            strcat(complete_buffer, buffer);
                        }
                    }
		    printf("out of loop\n");
                    
                    //end timing
                    struct timeval end_time;
                    gettimeofday(&end_time, NULL);

                    //check if the chunk type is video
                    //according to tut06pp10, Content-Type: video/f4f
                    //Analyze the reply and calculate the bitrate
		    //printf("type:\n%s\n",strstr(complete_buffer, "Content-Type:"));
                    if(strstr(complete_buffer, "Content-Type: video/f4f")!=NULL) {
		   	printf("chunk found\n");
                        double timeDiff=(end_time.tv_sec-chunk_start_time[i].tv_sec)+(end_time.tv_usec-chunk_start_time[i].tv_usec)/1000000.0;
                        double T_curN_new = (contentLength+headerLength) / timeDiff;
                        T_curN[i] = alpha * T_curN_new + (1 - alpha) * T_curN[i];
                        int bitrate = 0;
                        for(int j = 0; j < MAX_BITRATE_NUM; j++) {
                            if(T_curN[i] >= 1.5 * bitrates[i][j]) {
                                bitrate = bitrates[i][j];
                            }
                        }
			printf("tcurn[i]: %f\n",T_curN[i]);
			printf("bitrate[i]: %d\n",bitrate);
                        //write out the log
                        //ip,chunk name, server file, timediff, t_curn_new, t_curn[i], bitrate
                        //if open log fail
                        FILE *fp_log = fopen(log_file, "a");
                        if (fp_log == NULL) {
                            perror("open log file");
                            exit(1);
                        }
                        
                        int server_addr_index = i % server_num;
                        fprintf(fp_log, "%s %s %s %f %f %f %d\n", inet_ntoa(server_ips[i]), chunknames[i], ip_addresses[server_addr_index], timeDiff, T_curN_new, T_curN[i], bitrate);
                        fclose(fp_log);
                    }
		    else{
			printf("no chunk detected\n");
			}
                }
                        
            }
        }
        // =============END OF server socket======================
        // =======================================================
    }
    return 0;
}
