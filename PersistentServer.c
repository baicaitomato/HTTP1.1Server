#define __USE_XOPEN
#define _GNU_SOURCE
#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<unistd.h>
#include<time.h>
#include<netdb.h>
#include<fcntl.h>
#include<signal.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<sys/sendfile.h>
#define msg_size 1024
char *OK = "HTTP/1.1 200 OK\r\n";
char *Not_Modified = "HTTP/1.1 304 Not Modified\r\n\r\n";
char *Bad_Request = "HTTP/1.1 400 Bad Request\r\n\r\n";
char *Not_Found = "HTTP/1.1 404 File Not Found\r\n\r\n";
char *Precondition_Failed = "HTTP/1.1 412 Precondition Failed\r\n\r\n";
char *Unsupported_Media_Type = "HTTP/1.1 415 Unsupported Media Type\r\n\r\n";
char *Internal_Server_Error = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
char *Not_Implemented = "HTTP/1.1 501 Not Implemented\r\n\r\n";
char *HTTP_Version_Not_Supported = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
char *rfc_1036_format = "%a, %d %b %Y %T GMT";
char *rfc_1123_format = "%A, %d-%b-%y %T GMT";
char *file_type[6][2] = {
    {"txt", "text/plain" },
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"html","text/html" },
    {"jpg", "image/jpeg" },
    {"jpeg","image/jpeg"},
};


// cite:https://github.com/jamesbursa/httplint/blob/master/httplint.c
// cite:https://gist.github.com/laobubu/d6d0e9beb934b60b2e552c2d03e1409e
// cite:https://github.com/nathan78906/HTTPServer/blob/master/SimpleServer.c


const char *get_file_type(const char *absolut_path) {
    const char *dot = strrchr(absolut_path, '.');
    if (dot == absolut_path || !dot) {
       return "application/octet-stream"; // https://stackoverflow.com/questions/13509050/how-to-specify-mimetype-of-files-with-no-extension-in-nginx-config
    } // no extension file
    const char *file_ext = dot + 1;
    for (int i = 0; i < 6; i++) {
        if (strcasecmp(file_ext, file_type[i][0]) == 0) {
            return file_type[i][1];
        }
    }
    return "-1";
}

int handle_request(int client_fd, char *client_msg, char *root_path) {
    int file_fd, file_size;
    struct stat file_stat;
    char *key, *value;
    char *modified_time = "";
    char *unmodified_time = "";
    char *match_etag = "";
    int if_modified_since_in_header = 0;
    int if_unmodified_since_in_header = 0;
    int if_match_in_header = 0;
    int if_none_match_in_header = 0;
    int status = 0;

    // header error checking
    // check if request is GET
    if (strcasecmp(strtok(client_msg, " \t"), "GET") != 0) {
        write(client_fd, Not_Implemented, strlen(Not_Implemented));
        return status;
    }

    char *relative_path, *http_type;
    // check if path is empty
    if ((relative_path = strtok(NULL, " \t")) == NULL) {
        write(client_fd, Bad_Request, strlen(Bad_Request));
        return status;
    }

    // check if http version is right
    http_type = strtok(NULL, "\r\n");

    // check and set current status
    if (strcasecmp(http_type, "HTTP/1.1") == 0){
        status = 1;
    }else{
        status = -1;
    }

    if ((strcasecmp(http_type, "HTTP/1.0")) != 0 && (strcasecmp(http_type, "HTTP/1.1")) != 0) {
        write(client_fd, HTTP_Version_Not_Supported, strlen(HTTP_Version_Not_Supported));
        return status;
    }

    // check if clients need to check http response 304
    while(1){
        if ((key = strtok(NULL, " \t")) == NULL || (value = strtok(NULL, "\r\n")) == NULL) {
            break;
        }
        //fprintf(stdout, key);
        //fprintf(stdout, value);
        if (strcasecmp(key, "Connection:") == 0 || strcasecmp(key, "\nConnection:") == 0){ // check connection types
            if (strcasecmp(value, "close") == 0){
                status = -1;
            }else if (strcasecmp(value, "keep-alive") == 0){
                status = 1;
            }
        }
        // check if modifies since
        if (strcasecmp(key, "\nIf-Modified-Since:") == 0 || strcasecmp(key, "If-Modified-Since:") == 0) {
            //fprintf(stdout, key);
            modified_time = value;
            if_modified_since_in_header = 1;
        }
        if (strcasecmp(key, "\nIf-Unmodified-Since:") == 0 || strcasecmp(key, "If-Unmodified-Since:") == 0){
            unmodified_time = value;
            if_unmodified_since_in_header = 1;
        }
        if (strcasecmp(key, "\nIf-Match:") == 0 || strcasecmp(key, "If-Match:") == 0){
            match_etag = value;
            if_match_in_header = 1;
        }
        if (strcasecmp(key, "\nIf-None-Match:") == 0 || strcasecmp(key, "If-None-Match:") == 0){
            match_etag = value;
            if_none_match_in_header = 1;
        }
    }
    // header error checking

    char *absolut_path = (char *)malloc((strlen(root_path) + strlen(relative_path) + 1) * sizeof(char));
    memset(absolut_path, '\0', (strlen(root_path) + strlen(relative_path) + 1));
    strcat(absolut_path, root_path);

    // try to open the file
    // https://piazza.com/class/kxhl6o1cccn2oo?cid=68
    if (strcmp(relative_path, "/") == 0) {
        strcat(absolut_path, "/index.html");
    } else {
        strcat(absolut_path, relative_path);
    }
    if ((file_fd=open(absolut_path, O_RDONLY)) != -1 ) {
        if (fstat(file_fd, &file_stat) == -1) {
            write(client_fd, Internal_Server_Error, strlen(Internal_Server_Error));
            return status;
        }
        file_size = file_stat.st_size;

        // handle if-modified-since
        // https://developer.mozilla.org/zh-CN/docs/Web/HTTP/Headers/If-Modified-Since
        // https://github.com/jamesbursa/httplint/blob/master/httplint.c
        if (if_modified_since_in_header == 1) {
            struct tm tm; // cite:https://github.com/jamesbursa/httplint/blob/master/httplint.c line 885
            if (strptime(modified_time, "%c", &tm) != NULL // asctime
                || strptime(modified_time, rfc_1036_format, &tm) != NULL // "%a, %d %b %Y %T GMT"
                || strptime(modified_time, rfc_1123_format, &tm) != NULL) { // "%A, %d-%b-%y %T GMT"
                if (difftime(mktime(&tm), file_stat.st_mtime) >= 0) { // if modified time is not newer
                    //fprintf(stdout, "%f", difftime(mktime(&tm), file_stat.st_mtime));
                    //fprintf(stdout, "%f", difftime(file_stat.st_mtime, mktime(&tm)));
                    write(client_fd, Not_Modified, strlen(Not_Modified)); // https://developer.mozilla.org/zh-CN/docs/Web/HTTP/Headers/If-Modified-Since
                    return status;
                }
            }
            //fprintf("modified_time is NULL")
        }

        // handle if-unmodified-since
        if (if_unmodified_since_in_header == 1) {
            struct tm tm; // cite:https://github.com/jamesbursa/httplint/blob/master/httplint.c line 885
            if (strptime(unmodified_time, "%c", &tm) != NULL // asctime
                || strptime(unmodified_time, rfc_1036_format, &tm) != NULL // "%a, %d %b %Y %T GMT"
                || strptime(unmodified_time, rfc_1123_format, &tm) != NULL) { // "%A, %d-%b-%y %T GMT"
                //fprintf(stdout, "%f", difftime(mktime(&tm), file_stat.st_mtime));
                if (difftime(mktime(&tm), file_stat.st_mtime) < 0) { // if modified time is not newer 
                    write(client_fd, Precondition_Failed, strlen(Precondition_Failed));// https://developer.mozilla.org/zh-CN/docs/Web/HTTP/Headers/If-Modified-Since
                    return status;
                }
            }
        }

        char *etag;
        char etag_time[80];
        strftime(etag_time, 80, "%x%T", localtime(&(file_stat.st_mtime)));
        asprintf(&etag, "\"%ld-%s\"", (long)file_stat.st_size, etag_time);
        //fprintf(stdout, "etag:%s receive:%s \n", etag, match_etag);
        // check if match
        // https://developer.mozilla.org/zh-CN/docs/Web/HTTP/Headers/If-Match
        if (if_match_in_header == 1) {
            int etag_check_match = 0;
            if (strcmp(match_etag, "*") == 0) { // If-Match: *
                etag_check_match = 1;
            } else {
                char *etag_segment_match;
                etag_segment_match = strtok(match_etag, ", ");
                while(etag_segment_match != NULL) { // If-Match: <etag_value>, <etag_value>, ...
                    //fprintf(stdout, "%s", etag_segment_match);
                    if (strncmp(etag_segment_match, "W/", 2) == 0) { // If-Match: W/"67ab43", "54ed21", "7892dd"
                        etag_segment_match = etag_segment_match + 2;
                    }
                    if (strcmp(etag_segment_match, etag) == 0) {
                        etag_check_match = 1;
                        break;
                    }
                    etag_segment_match = strtok(NULL, ", ");
                }
            }
            if (etag_check_match == 0){
                write(client_fd, Precondition_Failed, strlen(Precondition_Failed));
                return status;
            }
        }

        // check if non match
        // https://developer.mozilla.org/zh-CN/docs/Web/HTTP/Headers/If-None-Match
        if (if_none_match_in_header == 1) {
            if (strcmp(match_etag, "*") != 0) { // If-None-Match: *
                char *etag_segment_none;
                etag_segment_none = strtok(match_etag, ", ");
                while(etag_segment_none != NULL) { // If-None-Match: <etag_value>, <etag_value>, ...
                    if (strncmp(etag_segment_none, "W/", 2) == 0) { // If-None-Match: W/""
                        etag_segment_none = etag_segment_none + 2;
                    }
                    if (strcmp(etag_segment_none, etag) == 0) {
                        write(client_fd, Precondition_Failed, strlen(Precondition_Failed));
                        return status;
                    }
                    etag_segment_none = strtok(NULL, ", ");
                }
            }
        }



        // check type
        const char *mime_type = get_file_type(absolut_path);
        if (strncmp(mime_type, "-1", 3) == 0) {
            write(client_fd, Unsupported_Media_Type, strlen(Unsupported_Media_Type));
            return status;
        }
        write(client_fd, OK, strlen(OK));

        // set last modified
        char rfc_time[80];
        char curr_time[80];
        time_t t = time(NULL);
        strftime(rfc_time, 80, rfc_1036_format, localtime(&(file_stat.st_mtime)));
        strftime(curr_time, 80, rfc_1036_format, localtime(&(t)));

        // send header
        char *header;
        if (strcasecmp(http_type, "HTTP/1.1") == 0){
            asprintf(&header, "Date: %s\r\nContent-Length: %d\r\nContent-Type: %s\r\nLast-Modified: %s\r\nETag: %s\r\n\r\n", curr_time, (int)file_size, mime_type, rfc_time, etag);
        } else {
            asprintf(&header, "Date: %s\r\nContent-Length: %d\r\nContent-Type: %s\r\nLast-Modified: %s\r\n\r\n", curr_time, (int)file_size, mime_type, rfc_time);
        }
        write(client_fd, header, strlen(header));
        free(header);

        // send file
        if (sendfile(client_fd, file_fd, NULL, file_size) == -1){
            write(client_fd, Internal_Server_Error, strlen(Internal_Server_Error));
            return status;
        }
        free(absolut_path);
        return status;
    } else {
        write(client_fd, Not_Found, strlen(Not_Found));
    }
    free(absolut_path);
    return status;
}


// PORT path
int main(int argc, char * argv[]){
    if (argc != 3){
        fprintf(stderr, "Please input %s 'PORT' 'ROOT'\n", argv[0]);
        exit(1);
    }

    int server_fd, client_fd, opt;
    struct sockaddr_in server_addr, client_addr;
    char client_msg[msg_size];
    int client_addr_len = sizeof(client_addr);
    memset(client_msg, '\0', msg_size);
    memset(&server_addr, 0, sizeof(struct sockaddr_in));

    // CSC209 TIMEEE
    // cite:our csc209 assignment
    // cite:https://www.geeksforgeeks.org/socket-programming-cc/
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(atoi(argv[1]));
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket:");
        exit(1);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, (socklen_t)sizeof(int)) < 0) {
        perror("setsockopt:");
        exit(1);
    }
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in))) {
        perror("bind:");
        exit(1);
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen:");
        exit(1);
    }
    fprintf(stdout, "Server %s:%d is starting to listen now!\n", inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));

    while(1){
        if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len)) < 0) {
            perror("accept:");
            exit(1);
        }

        int pid = fork();
        if (pid < 0){
            perror("fork:");
            exit(1);
        } else if (pid == 0){
            if (close(server_fd) < 0) {
                perror("close server fd for child process:");
                exit(1);
            }
            fprintf(stdout, "Server is connecting from client %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            // handle presistent http 1.1

            // set timeout to 5s
            struct timeval time;      
            time.tv_sec = 5;
            time.tv_usec = 0;
    
            if (setsockopt (client_fd, SOL_SOCKET, SO_RCVTIMEO, &time, sizeof time) < 0){
                perror("setsockopt failed\n");
                exit(1);
            }

            // set current status to 1 (connected)
            int status = 1;
                
            while (read(client_fd, client_msg, msg_size) > 0 && status == 1){
              fprintf(stdout, "Server is receiving from client\n%s\n", client_msg);
              status = handle_request(client_fd, client_msg, argv[2]); // update status
            }
            
            
            if (close(client_fd) < 0){
                perror("close client fd for child process:");
                exit(1);
            }
            return 0;
        } else {
            if (close(client_fd) < 0){
                perror("close client fd for parent process:");
                exit(1);
            }
        }
    }
    exit(1);
    return 0;
}
