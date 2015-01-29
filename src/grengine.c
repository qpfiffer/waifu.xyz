// vim: noet ts=4 sw=4
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"
#include "grengine.h"
#include "utils.h"
#include "logging.h"

static const char r_200[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: %s\r\n"
	"Content-Length: %zu\r\n"
	"Connection: close\r\n"
	"Server: waifu.xyz/bitch\r\n\r\n";

static const char r_404[] =
	"HTTP/1.1 404 Not Found\r\n"
	"Content-Type: %s\r\n"
	"Content-Length: %zu\r\n"
	"Connection: close\r\n"
	"Server: waifu.xyz/bitch\r\n\r\n";

static const char r_206[] =
	"HTTP/1.1 206 Partial Content\r\n"
	"Content-Type: %s\r\n"
	"Content-Length: %zu\r\n"
	"Accept-Ranges: bytes\r\n"
	"Content-Range: bytes %zu-%zu/%zu\r\n"
	"Connection: close\r\n"
	"Server: waifu.xyz/bitch\r\n\r\n";

/* This is used to map between the return codes of responses to their headers: */
static const code_to_message response_headers[] = {
	{200, r_200},
	{206, r_206},
	{404, r_404}
};

const code_to_message *get_response_headers() {
	return response_headers;
}

const size_t get_response_headers_num_elements() {
	return sizeof(response_headers)/sizeof(response_headers[0]);
}

int r_404_handler(const http_request *request, http_response *response) {
	response->out = (unsigned char *)"<h1>\"Welcome to Die|</h1>";
	response->outsize = strlen("<h1>\"Welcome to Die|</h1>");
	return 404;
}

void guess_mimetype(const char *ending, const size_t ending_siz, http_response *response) {
	/* This is how we do mimetypes. lol. */
	if (strncasecmp(ending, ".css", ending_siz) == 0) {
		strncpy(response->mimetype, "text/css", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".jpg", ending_siz) == 0) {
		strncpy(response->mimetype, "image/jpeg", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".txt", ending_siz) == 0) {
		strncpy(response->mimetype, "text/plain", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".html", ending_siz) == 0) {
		strncpy(response->mimetype, "text/html", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".ico", ending_siz) == 0) {
		strncpy(response->mimetype, "image/x-icon", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".webm", ending_siz) == 0) {
		strncpy(response->mimetype, "video/webm", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".gif", ending_siz) == 0) {
		strncpy(response->mimetype, "image/gif", sizeof(response->mimetype));
	} else if (strncasecmp(ending, ".js", ending_siz) == 0) {
		strncpy(response->mimetype, "text/javascript", sizeof(response->mimetype));
	} else {
		strncpy(response->mimetype, "application/octet-stream", sizeof(response->mimetype));
	}
}

int mmap_file(const char *file_path, const http_request *request, http_response *response) {
	return mmap_file_ol(file_path, request, response, NULL, NULL);
}

int mmap_file_ol(const char *file_path, const http_request *request, http_response *response,
				 const size_t *offset, const size_t *limit) {
	response->extra_data = calloc(1, sizeof(struct stat));

	if (stat(file_path, response->extra_data) == -1) {
		response->out = (unsigned char *)"<html><body><p>No such file.</p></body></html>";
		response->outsize= strlen("<html><body><p>No such file.</p></body></html>");
		free(response->extra_data);
		response->extra_data = NULL;
		return 404;
	}
	int fd = open(file_path, O_RDONLY);
	if (fd <= 0) {
		response->out = (unsigned char *)"<html><body><p>Could not open file.</p></body></html>";
		response->outsize= strlen("<html><body><p>could not open file.</p></body></html>");
		free(response->extra_data);
		response->extra_data = NULL;
		close(fd);
		return 404;
	}


	const struct stat st = *(struct stat *)response->extra_data;

	const size_t c_offset = offset != NULL ? *offset : 0;
	const size_t c_limit = limit != NULL ? (*limit - c_offset) : (st.st_size - c_offset);

	response->out = mmap(NULL, c_limit, PROT_READ, MAP_PRIVATE, fd, c_offset);
	response->outsize = c_limit - c_offset;

	if (response->out == MAP_FAILED) {
		char buf[128] = {0};
		perror(buf);
		log_msg(LOG_ERR, "Could not mmap file: %s", buf);

		response->out = (unsigned char *)"<html><body><p>Could not open file.</p></body></html>";
		response->outsize= strlen("<html><body><p>could not open file.</p></body></html>");
		close(fd);
		free(response->extra_data);
		response->extra_data = NULL;
		return 404;
	}
	close(fd);

	madvise(response->out, c_limit, MADV_SEQUENTIAL | MADV_WILLNEED);

	/* Figure out the mimetype for this resource: */
	char ending[16] = {0};
	int i = sizeof(ending);
	const size_t res_len = strlen(file_path);
	for (i = res_len; i > (res_len - sizeof(ending)); i--) {
		if (file_path[i] == '.')
			break;
	}
	strncpy(ending, file_path + i, sizeof(ending));
	guess_mimetype(ending, sizeof(ending), response);

	char *range_header_value = get_header_value(request->full_header, strlen(request->full_header), "Range");
	if (range_header_value) {
		range_header range = parse_range_header(range_header_value);
		free(range_header_value);

		log_msg(LOG_INFO, "Range header parsed: Limit: %zu Offset: %zu", range.limit, range.offset);
		memcpy(&response->byte_range, &range, sizeof(response->byte_range));

		return 206;
	}

	return 200;
}

void heap_cleanup(const int status_code, http_response *response) {
	if (status_code > 200 && status_code < 400)
		free(response->out);
}

void mmap_cleanup(const int status_code, http_response *response) {
	if (status_code > 200 && status_code < 400) {
		munmap(response->out, response->outsize);
		free(response->extra_data);
	}
}

int parse_request(const char to_read[MAX_READ_LEN], http_request *out) {
	/* Find the verb */
	const char *verb_end = strnstr(to_read, " ", MAX_READ_LEN);
	if (verb_end == NULL)
		goto error;

	const size_t verb_size = verb_end - to_read >= sizeof(out->verb) ? sizeof(out->verb) - 1: verb_end - to_read;
	strncpy(out->verb, to_read, verb_size);

	if (strncmp(out->verb, "GET", verb_size) != 0) {
		log_msg(LOG_WARN, "Don't know verb %s.", out->verb);
		goto error;
	}

	const char *res_offset = verb_end + sizeof(char);
	const char *resource_end = strnstr(res_offset, " ", sizeof(out->resource));
	if (resource_end == NULL)
		goto error;

	const size_t resource_size = resource_end - res_offset >= sizeof(out->resource) ? sizeof(out->resource) : resource_end - res_offset;
	strncpy(out->resource, res_offset, resource_size);

	return 0;

error:
	return -1;
}

int respond(const int accept_fd, const route *all_routes, const size_t route_num_elements) {
	char to_read[MAX_READ_LEN] = {0};
	char *actual_response = NULL;
	http_response response = {
		.mimetype = "text/html",
		.byte_range = {0},
		0
	};
	const route *matching_route = NULL;

	int rc = recv(accept_fd, to_read, MAX_READ_LEN, 0);
	if (rc <= 0) {
		log_msg(LOG_ERR, "Did not receive any information from accepted connection.");
		goto error;
	}

	/* Thanks, beej! */
	struct sockaddr_storage peer_addr = {0};
	socklen_t peer_addr_siz = sizeof(peer_addr);
	char ipstr[INET6_ADDRSTRLEN] = {0};
	getpeername(accept_fd, (struct sockaddr *)&peer_addr, &peer_addr_siz);

	struct sockaddr_in *s = (struct sockaddr_in *)&peer_addr;
	inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
	log_msg(LOG_FUN, "IP Address: %s", ipstr);

	http_request request = {
		.verb = {0},
		.resource = {0},
		.matches = {{0}},
		.full_header = to_read
	};
	rc = parse_request(to_read, &request);
	if (rc != 0) {
		log_msg(LOG_ERR, "Could not parse request.");
		goto error;
	}

	/* Find our matching route: */
	int i;
	for (i = 0; i < route_num_elements; i++) {
		const route *cur_route = &all_routes[i];
		if (strcmp(cur_route->verb, request.verb) != 0)
			continue;

		assert(cur_route->expected_matches < MAX_MATCHES);
		regex_t regex;
		int reti = regcomp(&regex, cur_route->route_match, REG_EXTENDED);
		if (reti != 0) {
			char errbuf[128];
			regerror(reti, &regex, errbuf, sizeof(errbuf));
			log_msg(LOG_ERR, "%s", errbuf);
			assert(reti == 0);
		}

		if (request.matches > 0)
			reti = regexec(&regex, request.resource, cur_route->expected_matches + 1, request.matches, 0);
		else
			reti = regexec(&regex, request.resource, 0, NULL, 0);
		regfree(&regex);
		if (reti == 0) {
			matching_route = &all_routes[i];
			break;
		}
	}

	/* If we didn't find one just use the 404 route: */
	if (matching_route == NULL)
		matching_route = &r_404_route;

	/* Run the handler through with the data we have: */
	const int response_code = matching_route->handler(&request, &response);
	assert(response.outsize > 0);
	assert(response.out != NULL);

	log_msg(LOG_FUN, "\"%s %s\" %i %i", request.verb, request.resource,
			response_code, response.outsize);

	/* Figure out what header we need to use: */
	const code_to_message *matched_response = NULL;
	const code_to_message *response_headers = get_response_headers();
	const unsigned int num_elements = get_response_headers_num_elements();
	for (i = 0; i < num_elements; i++) {
		code_to_message current_response = response_headers[i];
		if (current_response.code == response_code) {
			matched_response = &response_headers[i];
			break;
		}
	}
	/* Blow up if we don't have that code. We should have them all at
	 * compile time. */
	assert(matched_response != NULL);

	/* Embed the handler's text into the header: */
	size_t header_size = 0;
	size_t actual_response_siz = 0;

	if (response_code == 200 || response_code == 404) {
		const size_t integer_length = UINT_LEN(response.outsize);
		header_size = strlen(response.mimetype) + strlen(matched_response->message)
			+ integer_length - strlen("%s") - strlen("%zu");
		actual_response_siz = response.outsize + header_size;
		actual_response = calloc(1, actual_response_siz + 1);
		/* snprintf the header because it's just a string: */
		snprintf(actual_response, actual_response_siz, matched_response->message, response.mimetype, response.outsize);
		/* memcpy the rest because it could be anything: */
		memcpy(actual_response + header_size, response.out, response.outsize);
	} else if (response_code == 206) {
		/* Byte range queries have some extra shit. */
		const size_t c_offset = response.byte_range.offset;
		const size_t c_limit = response.byte_range.limit == 0 ?
			(response.outsize - c_offset) - 1 : (response.byte_range.limit - c_offset) - 1;
		const size_t full_size = c_limit + 1;
		const size_t integer_length = UINT_LEN(full_size);

		const size_t minb_len = c_offset == 0 ? 1 : UINT_LEN(c_offset);
		const size_t maxb_len = c_limit == 0 ? 1 : UINT_LEN(c_limit);
		header_size = strlen(response.mimetype) + strlen(matched_response->message)
			+ integer_length + minb_len + maxb_len + integer_length
			- strlen("%s") - (strlen("%zu") * 4);
		actual_response_siz = full_size + header_size;
		actual_response = calloc(1, actual_response_siz + 1);

		/* snprintf the header because it's just a string: */
		snprintf(actual_response, actual_response_siz, matched_response->message,
			response.mimetype, full_size,
			c_offset, c_limit, full_size);
		/* memcpy the rest because it could be anything: */
		memcpy(actual_response + header_size, response.out + c_offset, full_size);
	}

	/* Send that shit over the wire: */
	const size_t bytes_siz = actual_response_siz;
	rc = send(accept_fd, actual_response, bytes_siz, 0);
	if (rc <= 0) {
		log_msg(LOG_ERR, "Could not send response.");
		goto error;
	}
	if (matching_route->cleanup != NULL)
		matching_route->cleanup(response_code, &response);
	free(actual_response);

	return 0;

error:
	if (matching_route != NULL)
		matching_route->cleanup(500, &response);
	free(actual_response);
	return -1;
}
