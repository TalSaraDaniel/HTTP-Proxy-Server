#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
    char protocol[10];
    char host[256];
    int port;
    char path[256];
} ParsedURL;

void parseURL(const char *url, ParsedURL *result) {
    // Assuming url starts with "http://"
    const char *prefix = "http://";
    const char *hostStart = strstr(url, prefix) + strlen(prefix);
    const char *temp = strchr(hostStart, '/');
    char pathStart[1024];
    memset(pathStart, 0, 1024);

    // Extract protocol
    strncpy(result->protocol, prefix, sizeof(result->protocol) - 1);
    result->protocol[sizeof(result->protocol) - 1] = '\0';

    // Extract host
    const char *portColon = strchr(hostStart, ':');
    if (portColon != NULL) {
        // If port is specified, extract it and update host
        result->port = atoi(portColon + 1);
        strncpy(result->host, hostStart, MIN(portColon - hostStart, sizeof(result->host) - 1));
        result->host[MIN(portColon - hostStart, sizeof(result->host) - 1)] = '\0';
    } else {
        // If port is not specified, copy until the next '/' or end of string
        size_t hostLength = (temp != NULL) ? MIN(temp - hostStart, sizeof(result->host) - 1) : strlen(hostStart);
        strncpy(result->host, hostStart, hostLength);
        result->host[hostLength] = '\0';
        result->port = 80; // Default port for HTTP
    }


    // Extract path
    memset(result->path, 0, sizeof(result->path));
    if ((temp != NULL) && (strcmp(temp, "/") != 0)) {
        strncpy(result->path, temp, sizeof(result->path) - 1);
        result->path[sizeof(result->path) - 1] = '\0';
        strcat(pathStart, temp);
    } else {
        strcat(result->path, "/index.html");
    }
}

int fileExists(const ParsedURL *parsedURL) {
    char localDirectory[1024];
    char filePath[1024];
    memset(localDirectory, 0, 1024);
    memset(filePath, 0, 1024);

    // Concatenate local directory, host, and path to form a complete local file path
    snprintf(filePath, sizeof(filePath), "%s%s", parsedURL->host, parsedURL->path);

    if (access(filePath, F_OK) != -1) {
        // File exists
        return 1;
    } else {
        // File does not exist
        return 0;
    }
}

void constructHTTPRequest(const ParsedURL *parsedURL, char *httpRequest) {
    sprintf(httpRequest, "GET /%s HTTP/1.0\r\nHost: %s\r\n\r\n", parsedURL->path, parsedURL->host);
    httpRequest[511] = '\0';  // Ensure null-termination
}

void createDirectories(const ParsedURL *parsedURL) {
    char *prev;
    // Combine the components to create the directory path
    char directoryPath[512];
    memset(directoryPath, 0, sizeof(directoryPath));
    snprintf(directoryPath, sizeof(directoryPath), "%s/%s", parsedURL->host, parsedURL->path + 1);
    // Create each directory in the path
    char *token = strtok(directoryPath, "/");
    char currentPath[512] = "";

    while (token != NULL) {
        prev = token;
        token = strtok(NULL, "/");
        if (token != NULL) {
            strcat(currentPath, prev);
            strcat(currentPath, "/");
            // Create the directory if it doesn't exist
            if (mkdir(currentPath, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
                // Directory creation failed
                perror("Failed to create directory");
                //exit(EXIT_FAILURE);
            }
        }
    }
}

FILE *openFile(const char *filePath) {
    FILE *file = fopen(filePath, "w+");

    if (file == NULL) {
        perror("Error opening file");
    }
    return file;
}

long receiveAndSaveResponse(int sockfd, FILE *file, int *statusCode) {
    unsigned char buffer[4096];
    memset(buffer, 0, sizeof(buffer));
    ssize_t bytesRead;
    int headerEnd = 0;  // Flag to detect the end of headers
    long counter = 0;   // Initialize counter

    // Read and print the response from the server
    while ((bytesRead = read(sockfd, buffer, sizeof(buffer))) > 0) {
        if (bytesRead < 0) {
            perror("Error reading from server");
            return -1;
        }

        // Search for the end of headers
        if (!headerEnd) {
            // Manual search for "\r\n\r\n"
            for (size_t i = 0; i < bytesRead - 3; i++) {
                if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                    headerEnd = 1;
                    size_t bytesAfterHeaders = bytesRead - (i + 4);
                    if (bytesAfterHeaders > 0) {
                        fwrite(buffer + i + 4, 1, bytesAfterHeaders, file);  // Write the body to file
                    }
                    break;
                }
            }
        } else {
            // Write the body to file
            fwrite(buffer, 1, bytesRead, file);
        }

        // Print to screen and update counter for characters in both the header and body
        fwrite(buffer, 1, bytesRead, stdout);
        fflush(stdout);  // Ensure stdout is flushed
        counter += bytesRead;

        // Check for the status code in the HTTP response for HTTP/1.0 or HTTP/1.1
        if (counter >= 15 &&
            (strncmp((char *) buffer, "HTTP/1.0 200", 12) == 0 || strncmp((char *) buffer, "HTTP/1.1 200", 12) == 0)) {
            *statusCode = atoi((char *) (buffer + 9));
        }

        memset(buffer, 0, 4096);
    }

    return counter;  // Return the total number of characters printed to stdout
}

// Function to create an HTTP response string with the specified content length
void createHTTPResponse(size_t contentLength, char *response, size_t responseSize) {
    snprintf(response, responseSize, "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n", contentLength);
}

// Function to get the content length of a file
size_t getFileSize(const char *filename) {
    FILE *file = fopen(filename, "rb");  // Open the file in binary mode for reading
    if (file == NULL) {
        perror("Error opening file");
        return 0;  // Return 0 on error
    }

    // Move the file cursor to the end of the file
    if (fseek(file, 0, SEEK_END) != 0) {
        perror("Error seeking file");
        fclose(file);
        return 0;  // Return 0 on error
    }

    // Get the current position of the file cursor, which gives the file size
    long fileSize = ftell(file);
    if (fileSize == -1) {
        perror("Error getting file size");
        fclose(file);
        return 0;  // Return 0 on error
    }

    // Rewind the file cursor to the beginning of the file
    rewind(file);

    fclose(file);
    return (size_t) fileSize;  // Convert to size_t and return
}


int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage: cproxy <URL> [-s]");
        exit(EXIT_FAILURE);
    }
    char *url = argv[1];
    char *flag = argv[2];

    //create a structure
    ParsedURL parsedResult;
    parseURL(url, &parsedResult); //parsing the url


    if (fileExists(&parsedResult)) {    //check if the file is already in the file system
        char directoryPath[512];
        memset(directoryPath, 0, sizeof(directoryPath));
        snprintf(directoryPath, sizeof(directoryPath), "%s/%s", parsedResult.host, parsedResult.path + 1);
        size_t size = getFileSize(directoryPath);


        char *buf1 = "HTTP/1.0 200 OK\r\n";
        int len = strlen(buf1);
        printf("File is given from local filesystem\n");
        char buf2[128];
        memset(buf2, 0, sizeof(buf2));
        len += snprintf(buf2, 128, "Content-Length: %zu\r\n\r\n", size);


        char response[1024];
        createHTTPResponse(size, response, sizeof(response));
        printf("%s", response);


        FILE *file = fopen(directoryPath, "rb");  // Open the file in binary mode for reading
        if (file == NULL) {
            perror("Error opening file");
            return 0;  // Return 0 on error
        }
        int character;
        while ((character = fgetc(file)) != EOF) {
            putchar(character);  // Print each character to the screen
        }

        //handle the s flag
        char firefoxString[1024];
        memset(firefoxString, 0, 1024);
        // Create the string containing the word "Firefox"
        sprintf(firefoxString, "firefox ");

        // Concatenate parsedResult.host and parsedResult.path to firefoxString
        strcat(firefoxString, parsedResult.host);
        strcat(firefoxString, parsedResult.path);
        if (flag != NULL && strcmp(flag, "-s") == 0) {
            system(firefoxString);
        }
        printf("\n Total response bytes: %zu\n", size + len);
        fclose(file);

    } else {    //in case the file isn't in the file system

        //powerpoint
        in_port_t port = 80;
        int sockfd = -1;
        struct hostent *server_info = NULL;

        // Create a socket with the address format of IPV4 over TCP
        if ((sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
            perror("socket failed");
            exit(EXIT_FAILURE);
        }

        // Use gethostbyname to translate host name to network byte order ip address
        server_info = gethostbyname(parsedResult.host);
        if (!server_info) {
            herror("gethostbyname failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // Initialize sockaddr_in structure
        struct sockaddr_in sock_info;

        // Set its attributes to 0 to avoid undefined behavior
        memset(&sock_info, 0, sizeof(struct sockaddr_in));

        // Set the type of the address to be IPV4
        sock_info.sin_family = AF_INET;

        // Set the socket's port
        sock_info.sin_port = htons(port);

        // Set the socket's ip
        sock_info.sin_addr.s_addr = ((struct in_addr *) server_info->h_addr)->s_addr;

        // Connect to the server
        if (connect(sockfd, (struct sockaddr *) &sock_info, sizeof(struct sockaddr_in)) == -1) {
            perror("connect failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        //end of PowerPoint

        // Construct and print the HTTP request
        char httpRequest[512];
        memset(httpRequest, 0, 512);
        constructHTTPRequest(&parsedResult, httpRequest);
        printf("HTTP request =\n%s\nLEN = %lo\n", httpRequest, strlen(httpRequest));

        // create directories
        createDirectories(&parsedResult);

        char concatenatedPath[1024]; // Adjust the size according to your needs
        memset(concatenatedPath, 0, 1024);
        sprintf(concatenatedPath, "%s%s", parsedResult.host, parsedResult.path);

        FILE *file = openFile(concatenatedPath);
        if (file == NULL) {
            printf("openFile");
            close(sockfd);
            fclose(file);
            exit(EXIT_FAILURE);
        }


        if ((write(sockfd, httpRequest, sizeof(httpRequest))) < 0) {
            perror("write");
            exit(1);
        }

        int statusCode;
        long responseSize = receiveAndSaveResponse(sockfd, file, &statusCode);
        // Receive and save the server response
        if (responseSize == -1) {
            exit(EXIT_FAILURE);
        }

        if (statusCode != 200) {
            // Delete the file if it exists
            if (file != NULL) {
                fclose(file);  // Close the file before deletion
                if (remove(concatenatedPath) == 0) {
                    printf("File deleted successfully.\n");
                } else {
                    perror("Error deleting file");
                }
            }
        }

        //handle the s flag
        char firefoxString[1024];
        memset(firefoxString, 0, 1024);
        // Create the string containing the word "Firefox"
        sprintf(firefoxString, "firefox ");

        // Concatenate parsedResult.host and parsedResult.path to firefoxString
        strcat(firefoxString, parsedResult.host);
        strcat(firefoxString, parsedResult.path);
        if (flag != NULL && strcmp(flag, "-s") == 0) {
            system(firefoxString);
        }
        printf("\n Total response bytes: %ld\n", responseSize);

        // Close the socket after successful connection
        close(sockfd);
        fclose(file);
    }
}