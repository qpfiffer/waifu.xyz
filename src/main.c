/* vim: noet ts=4 sw=4 */
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>

#define DEBUG 0

const char FOURCHAN_API_HOST[] = "a.4cdn.org";
const char FOURCHAN_API_URL[] = "http://a.4cdn.org/b/catalog.json";
int main_sock_fd = 0;

int new_API_request() {
	struct addrinfo hints = {0};
	struct addrinfo *res = NULL;
	int request_fd;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	getaddrinfo(FOURCHAN_API_HOST, "80", &hints, &res);

	request_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (request_fd < 0)
		return -1;

	// connect!
	int rc = connect(request_fd, res->ai_addr, res->ai_addrlen);
	if (rc == -1) {
		close(request_fd);
		return -1;
	}
	printf("Connected to 4chan.\n");

	close(request_fd);
	return 0;
}

void background_work(int debug) {
	printf("BGWorker chuggin'\n");
	if (new_API_request() != 0)
		return;
}

int start_bg_worker(int debug) {
	pid_t PID = fork();
	if (PID == 0) {
		background_work(debug);
	} else if (PID < 0) {
		return -1;
	}
	return 0;
}

int http_serve() {
	main_sock_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (main_sock_fd < 0)
		return -1;

	struct sockaddr_in hints = {0};
	hints.sin_family		 = AF_INET;
	hints.sin_port			 = htons(8080);
	hints.sin_addr.s_addr	 = htonl(INADDR_LOOPBACK);

	int rc = bind(main_sock_fd, (struct sockaddr *)&hints, sizeof(hints));
	if (rc < 0)
		return -1;

	rc = listen(main_sock_fd, 0);
	if (rc < 0)
		return -1;

	close(main_sock_fd);
	return 0;
}

int main(int argc, char *argv[]) {
	if (start_bg_worker(DEBUG) != 0)
		return -1;
	if (http_serve() != 0)
		return -1;
	return 0;
}
