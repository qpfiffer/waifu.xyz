// vim: noet ts=4 sw=4
#ifdef __clang__
	#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <regex.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include <38-moths/38-moths.h>

#include "db.h"
#include "http.h"
#include "parse.h"
#include "parson.h"
#include "models.h"
#include "server.h"

#define RESULTS_PER_PAGE 160
#define OFFSET_FOR_PAGE(x) x * RESULTS_PER_PAGE

static char *pretty_date(const char *argument) {
	char *buf = NULL;
	if (!argument || strnlen(argument, 64) == 0)
		goto error;

	const time_t tim = strtol(argument, NULL, 10);

	if (tim < 0)
		goto error;

	if ((tim == LONG_MIN || tim == LONG_MAX) && errno == ERANGE)
		return strdup(argument);

	const time_t non_milli_tim = tim;

	struct tm converted;
	gmtime_r(&non_milli_tim, &converted);

	buf = calloc(25, sizeof(char));
    strftime(buf, 25, "%F(%a)%T", &converted);

	return buf;

error:
	buf = calloc(1, sizeof(char) * 4);
	strcpy(buf, "N/A");

	return buf;
}

static char *thumbnail_for_image(const char *argument) {
	const size_t arg_len = strlen(argument);
	const size_t stop_at = arg_len - strlen("webm");

	const char prefix[] = "t/thumb_";
	const size_t prefix_siz = strlen(prefix);

	char *to_return = calloc(1, stop_at + strlen("jpg") + prefix_siz + 1);
	strncpy(to_return, prefix, prefix_siz);
	strncat(to_return, argument, stop_at);
	strncat(to_return, "jpg", strlen("jpg"));
	return to_return;
}

static inline int alphabetical_cmp(const void *a, const void *b) {
	return strncmp((char *)a, (char *)b, MAX_IMAGE_FILENAME_SIZE);
}

static inline int compare_dates(const void *a, const void *b) {
	const struct file_and_time *_a = a;
	const struct file_and_time *_b = b;

	return _b->ctime - _a->ctime;
}

static int _add_webms_in_dir_by_date(greshunkel_var *loop, const char *dir,
		const unsigned int offset, const unsigned int limit) {
	/* Check to make sure the directory actually exists. */
	struct stat dir_st = {0};
	if (stat(dir, &dir_st) == -1)
		return 0;

	DIR *dirstream = opendir(dir);
	unsigned int total = 0;
	vector *webm_vec = vector_new(sizeof(struct file_and_time), 2048);

	while (1) {
		struct dirent *result = readdir(dirstream);
		if (!result)
			break;

		if (result->d_name[0] != '.' && endswith(result->d_name, ".webm")) {
			struct stat st = {0};
			char *full_path = get_full_path_for_file(dir, result->d_name);
			if (stat(full_path, &st) == -1) {
				m38_log_msg(LOG_ERR, "Could not stat file: %s", result->d_name);
				free(full_path);
				continue;
			}

			struct file_and_time new = {
				.fname = {0},
				.ctime = st.st_mtime
			};
			strncpy(new.fname, result->d_name, sizeof(new.fname));
			vector_append(webm_vec, &new, sizeof(struct file_and_time));
			free(full_path);
			total++;
		}
	}
	closedir(dirstream);

	if (webm_vec->count <= 0) {
		vector_free(webm_vec);
		return 0;
	}

	qsort(webm_vec->items, webm_vec->count, webm_vec->item_size, &compare_dates);
	uint64_t i;
	for (i = 0; i < webm_vec->count; i++) {
		int8_t can_add = 0;
		if (!limit && !offset)
			can_add = 1;
		else if (i >= offset && i < (offset + limit))
			can_add = 1;

		if (can_add) {
			const struct file_and_time *x = vector_get(webm_vec, i);
			gshkl_add_string_to_loop(loop, x->fname);
		}
	}

	vector_free(webm_vec);
	return total;
}

static int _add_files_in_dir_to_arr(greshunkel_var *loop, const char *dir) {
	vector *alphabetical_vec = vector_new(MAX_IMAGE_FILENAME_SIZE, 16);

	DIR *dirstream = opendir(dir);
	unsigned int total = 0;
	while (1) {
		struct dirent *result = readdir(dirstream);
		if (!result)
			break;

		if (result->d_name[0] != '.') {
			vector_append(alphabetical_vec, result->d_name, strnlen(result->d_name, MAX_IMAGE_FILENAME_SIZE));
			total++;
		}
	}
	closedir(dirstream);

	if (alphabetical_vec->count <= 0) {
		vector_free(alphabetical_vec);
		return 0;
	}

	qsort(alphabetical_vec->items, alphabetical_vec->count, alphabetical_vec->item_size, &alphabetical_cmp);
	uint64_t i;
	for (i = 0; i < alphabetical_vec->count; i++) {
		const char *name = vector_get(alphabetical_vec, i);
		gshkl_add_string_to_loop(loop, name);
	}

	vector_free(alphabetical_vec);
	return total;
}

int static_handler(const m38_http_request *request, m38_http_response *response) {
	/* Remove the leading slash: */
	const char *file_path = request->resource + sizeof(char);
	return m38_mmap_file(file_path, response);
}

int user_thumbs_static_handler(const m38_http_request *request, m38_http_response *response) {
	/* Remove the leading slash: */
	const char *file_path = request->resource + sizeof(char);
	char buf[256] = {0};
	snprintf(buf, sizeof(buf), "./user_uploaded/t/%s", file_path);
	return m38_mmap_file(buf, response);
}

static void get_current_board(char current_board[static MAX_BOARD_NAME_SIZE], const m38_http_request *request) {
	const size_t board_len = request->matches[1].rm_eo - request->matches[1].rm_so;
	const size_t bgr = MAX_BOARD_NAME_SIZE > board_len ? board_len : MAX_BOARD_NAME_SIZE;
	strncpy(current_board, request->resource + request->matches[1].rm_so, bgr);
}

static void get_webm_from_board(char file_name_decoded[static MAX_IMAGE_FILENAME_SIZE], const m38_http_request *request) {
	char file_name[MAX_IMAGE_FILENAME_SIZE] = {0};
	char file_name_decoded_first_pass[MAX_IMAGE_FILENAME_SIZE] = {0};
	const size_t file_name_len = request->matches[2].rm_eo - request->matches[2].rm_so;
	const size_t fname_bgr = sizeof(file_name) > file_name_len ? file_name_len : sizeof(file_name);
	const size_t safe = fname_bgr > sizeof(file_name) ? sizeof(file_name) : fname_bgr;
	strncpy(file_name, request->resource + request->matches[2].rm_so, safe);

	url_decode(file_name, file_name_len, file_name_decoded_first_pass);
	/* Fuck it. */
	unsigned int i = 0;
	unsigned int j = 0;
	for (;i < strnlen(file_name, MAX_IMAGE_FILENAME_SIZE); i++) {
		/* TODO: Handle " as well. */
		if (file_name_decoded_first_pass[i] == '\'') {
			if (j + 6 > strnlen(file_name_decoded, MAX_IMAGE_FILENAME_SIZE)) {
				file_name_decoded[j++] = '\0';
				break;
			}
			/* &#039; */
			file_name_decoded[j++] = '&';
			file_name_decoded[j++] = '#';
			file_name_decoded[j++] = '0';
			file_name_decoded[j++] = '3';
			file_name_decoded[j++] = '9';
			file_name_decoded[j++] = ';';
		} else {
			file_name_decoded[j++] = file_name_decoded_first_pass[i];
		}
	}
}

int board_static_handler(const m38_http_request *request, m38_http_response *response) {
	const char *webm_loc = webm_location();

	/* Current board */
	char current_board[MAX_BOARD_NAME_SIZE] = {0};
	get_current_board(current_board, request);

	/* Filename after url decoding. */
	char file_name_decoded[MAX_IMAGE_FILENAME_SIZE] = {0};
	get_webm_from_board(file_name_decoded, request);

	/* Open and mmap() the file. */
	const size_t full_path_size = strlen(webm_loc) + strlen("/") +
								  strlen(current_board) + strlen("/") +
								  strlen(file_name_decoded) + 1;
	char full_path[full_path_size + 1];
	memset(full_path, '\0', sizeof(full_path));
	snprintf(full_path, full_path_size, "%s/%s/%s", webm_loc, current_board, file_name_decoded);

	return m38_mmap_file(full_path, response);
}

int index_handler(const m38_http_request *request, m38_http_response *response) {
	UNUSED(request);
	/* Render that shit */
	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_int(ctext, "webm_count", webm_count());
	gshkl_add_int(ctext, "alias_count", webm_alias_count());
	gshkl_add_int(ctext, "post_count", post_count());

	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());
	return m38_render_file(ctext, "./templates/index.html", response);
}

static int _api_failure(m38_http_response *response, greshunkel_ctext *ctext, const char *error) {
	gshkl_add_string(ctext, "SUCCESS", "false");
	gshkl_add_string(ctext, "ERROR", error);
	gshkl_add_string(ctext, "DATA", "{}");
	return m38_render_file(ctext, "./templates/response.json", response);
}

int url_search_handler(const m38_http_request *request, m38_http_response *response) {
	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_filter(ctext, "pretty_date", &pretty_date, &gshkl_filter_cleanup);
	gshkl_add_filter(ctext, "thumbnail_for_image", &thumbnail_for_image, &gshkl_filter_cleanup);

	/* All boards */
	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());

	const unsigned char *full_body = request->full_body;
	JSON_Value *body_string = json_parse_string((const char *)full_body);
	if (!body_string)
		return _api_failure(response, ctext, "Could not parse JSON object.");

	JSON_Object *webm_url_object = json_value_get_object(body_string);
	if (!webm_url_object) {
		json_value_free(body_string);
		return _api_failure(response, ctext, "Could not get object from JSON.");
	}

	const char *webm_url = json_object_get_string(webm_url_object, "webm_url");

	if (!webm_url) {
		json_value_free(body_string);
		return _api_failure(response, ctext, "'webm_url' is a required key.");
	}

	/* Download the file. */
	const char *filename = strrchr(webm_url, '/');
	if (filename == NULL) {
		json_value_free(body_string);
		return _api_failure(response, ctext, "Bogus filename in URL.");
	}

	char filename_to_write[MAX_IMAGE_FILENAME_SIZE] = {0};
	char out_filepath[MAX_IMAGE_FILENAME_SIZE] = {0};
	snprintf(filename_to_write, sizeof(filename_to_write), "%i_%s", rand(), filename + sizeof(char));

	const size_t new_webm_size = download_sent_webm_url(webm_url, filename_to_write, out_filepath);
	json_value_free(body_string);

	if (new_webm_size == 0)
		return _api_failure(response, ctext, "Could not download webm.");

	/* Hash the file. */
	char image_hash[HASH_IMAGE_STR_SIZE] = {0};
	char webm_key[MAX_KEY_SIZE] = {0};
	hash_file(out_filepath, image_hash);
	webm *_webm = get_image_by_oleg_key(image_hash, webm_key);

	char alias_key[MAX_KEY_SIZE] = {0};
	webm_alias *_alias = get_aliased_image_by_oleg_key(out_filepath, alias_key);

	greshunkel_var results = gshkl_add_array(ctext, "RESULTS");

	/* This code is fucking terrible. */
	int found = 0;
	if (_webm) {
		found = 1;

		greshunkel_ctext *result = gshkl_init_context();
		gshkl_add_string(result, "thumbnail", _webm->filename);
		gshkl_add_string(result, "filename", _webm->filename);
		post *_post = get_post(_webm->post_id);
		if (_post) {
			gshkl_add_int(result, "thread_id", _post->thread_id);
			gshkl_add_int(result, "post_id", _post->fourchan_post_id);
			if (_post->body_content) {
				gshkl_add_string(result, "post_content", _post->body_content);
				free(_post->body_content);
			} else {
				gshkl_add_string(result, "post_content", "...");
			}
			vector_free(_post->replied_to_keys);
		} else {
			gshkl_add_string(result, "post_content", "...");
			gshkl_add_string(result, "post_id", "...");
			gshkl_add_string(result, "thread_id", "...");
		}
		free(_post);
		gshkl_add_string(result, "board", _webm->board);

		gshkl_add_string(result, "is_last", "asdf");
		gshkl_add_sub_context_to_loop(&results, result);
	}

	if (_alias) {
		found = 1;
		/* TODO: Loop through webms and aliases here */
		//greshunkel_ctext *result = gshkl_init_context();
		//gshkl_add_string(result, "is_last", "asdf");
		//gshkl_add_sub_context_to_loop(&results, result);
	}

	free(_webm);
	free(_alias);

	if (found) {
		gshkl_add_string(ctext, "SUCCESS", "true");
		gshkl_add_string(ctext, "ERROR", NULL);
		return m38_render_file(ctext, "./templates/response_with_results.json", response);
	}

	gshkl_add_string(ctext, "SUCCESS", "true");
	gshkl_add_string(ctext, "ERROR", NULL);
	gshkl_add_string(ctext, "DATA", "[]");

	return m38_render_file(ctext, "./templates/response.json", response);
}

int webm_handler(const m38_http_request *request, m38_http_response *response) {
	char current_board[MAX_BOARD_NAME_SIZE] = {0};
	get_current_board(current_board, request);

	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_string(ctext, "current_board", current_board);
	gshkl_add_filter(ctext, "pretty_date", &pretty_date, &gshkl_filter_cleanup);

	/* All boards */
	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());

	/* Decode the url-encoded filename. */
	char file_name_decoded[MAX_IMAGE_FILENAME_SIZE] = {0};
	get_webm_from_board(file_name_decoded, request);
	gshkl_add_string(ctext, "image", file_name_decoded);

	/* Full path, needed for the image hash */
	char *full_path = get_full_path_for_webm(current_board, file_name_decoded);

	greshunkel_var aliases = gshkl_add_array(ctext, "aliases");
	char image_hash[HASH_IMAGE_STR_SIZE] = {0};

	char webm_key[MAX_KEY_SIZE] = {0};
	hash_file(full_path, image_hash);
	webm *_webm = get_image_by_oleg_key(image_hash, webm_key);

	char alias_key[MAX_KEY_SIZE] = {0};
	webm_alias *_alias = get_aliased_image_by_oleg_key(full_path, alias_key);

	/* This code is fucking terrible. */
	int found = 0;
	if (_webm) {
		found = 1;
		if (_alias) {
			/* This shouldn't happen, but whatever. */
			free(_alias);
		}
	} else if (_alias) {
		found = 1;
		free(_webm);
		_webm = (webm *)_alias;
	}

	if (!found) {
		gshkl_add_string(ctext, "image_date", NULL);
		gshkl_add_string(ctext, "post_content", NULL);
		gshkl_add_string(ctext, "post_id", NULL);
		gshkl_add_string(ctext, "thread_id", NULL);
	} else {
		post *_post = get_post(_webm->post_id);
		if (_post) {
			gshkl_add_int(ctext, "thread_id", _post->thread_id);
			gshkl_add_int(ctext, "post_id", _post->fourchan_post_id);
			if (_post->body_content) {
				gshkl_add_string(ctext, "post_content", _post->body_content);
				free(_post->body_content);
			} else {
				gshkl_add_string(ctext, "post_content", NULL);
			}
			vector_free(_post->replied_to_keys);
		} else {
			gshkl_add_string(ctext, "post_content", NULL);
			gshkl_add_string(ctext, "post_id", NULL);
			gshkl_add_string(ctext, "thread_id", NULL);
		}
		free(_post);
		time_t earliest_date = _webm->created_at;

		/* Add known aliases from DB. We fetch every alias from the M2M,
		 * and then fetch that key. Or try to, anyway. */
		PGresult *res = get_aliases_by_webm_id(_webm->id);
		unsigned int total_rows = 0;
		if (res) {
			unsigned int i = 0;
			total_rows = PQntuples(res);
			if (total_rows == 0) {
				gshkl_add_string_to_loop(&aliases, "None");
			}
			for (i = 0; i < total_rows; i++) {
				webm_alias *dsrlzd = deserialize_alias_from_tuples(res, i);

				if (dsrlzd) {
					if (dsrlzd->created_at < earliest_date)
						earliest_date = dsrlzd->created_at;
					const size_t buf_size = UINT_LEN(dsrlzd->created_at) + strlen(", ") +
						strnlen(dsrlzd->board, MAX_BOARD_NAME_SIZE) + strlen(", ") +
						strnlen(dsrlzd->filename, MAX_IMAGE_FILENAME_SIZE);
					char buf[buf_size + 1];
					buf[buf_size] = '\0';
					snprintf(buf, buf_size, "%lld, %s, %s", (long long)dsrlzd->created_at, dsrlzd->board, dsrlzd->filename);
					gshkl_add_string_to_loop(&aliases, buf);
					free(dsrlzd);
				}
			}
			PQclear(res);
		} else {
			gshkl_add_string_to_loop(&aliases, "None");
		}

		gshkl_add_int(ctext, "image_date", earliest_date);
	}

	free(_webm);
	free(full_path);
	return m38_render_file(ctext, "./templates/webm.html", response);
}

static int _board_handler(const m38_http_request *request, m38_http_response *response, const unsigned int page) {
	char current_board[MAX_BOARD_NAME_SIZE] = {0};
	get_current_board(current_board, request);

	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_filter(ctext, "thumbnail_for_image", thumbnail_for_image, gshkl_filter_cleanup);
	gshkl_add_string(ctext, "current_board", current_board);
	greshunkel_var images = gshkl_add_array(ctext, "IMAGES");

	char images_dir[256] = {0};
	snprintf(images_dir, sizeof(images_dir), "%s/%s", webm_location(), current_board);

	/* Check to make sure the directory actually exists. */
	struct stat dir_st = {0};
	if (stat(images_dir, &dir_st) == -1)
		return 404;

	int total = _add_webms_in_dir_by_date(&images, images_dir,
			OFFSET_FOR_PAGE(page), RESULTS_PER_PAGE);

	greshunkel_var pages = gshkl_add_array(ctext, "PAGES");
	unsigned int i;
	const unsigned int max = total/RESULTS_PER_PAGE;
	for (i = 0; i < max; i++)
		gshkl_add_int_to_loop(&pages, i);

	if (page > 0) {
		gshkl_add_int(ctext, "prev_page", page - 1);
	} else {
		gshkl_add_string(ctext, "prev_page", "");
	}

	if (page < max) {
		gshkl_add_int(ctext, "next_page", page + 1);
	} else {
		gshkl_add_string(ctext, "next_page", "");
	}

	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());

	gshkl_add_int(ctext, "total", total);

	return m38_render_file(ctext, "./templates/board.html", response);
}

static unsigned int _add_sorted_by_aliases(greshunkel_var *images,
		const unsigned int offset, const unsigned int limit) {
	PGresult *res = get_images_by_popularity(offset, limit);
	unsigned int total_rows = 0;
	if (res) {
		unsigned int i = 0;
		total_rows = PQntuples(res);
		for (i = 0; i < total_rows; i++) {
			webm *dsrlzd = deserialize_webm_from_tuples(res, i);

			if (dsrlzd) {
				greshunkel_ctext *_webm_sub = gshkl_init_context();

				gshkl_add_string(_webm_sub, "filename", dsrlzd->filename);
				gshkl_add_string(_webm_sub, "board", dsrlzd->board);

				gshkl_add_sub_context_to_loop(images, _webm_sub);
			}

			free(dsrlzd);
		}
	}

	PQclear(res);

	return total_rows;
}

int by_thread_handler(const m38_http_request *request, m38_http_response *response) {
	char thread_id[256] = {0};
	strncpy(thread_id, request->resource + request->matches[1].rm_so, sizeof(thread_id));

	if (thread_id == NULL || request->resource + request->matches[1].rm_so == 0)
		return 404;

	thread *_thread = get_thread_by_id(atol(thread_id));
	if (_thread == NULL)
		return 404;

	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_string(ctext, "thread_id", thread_id);
	gshkl_add_filter(ctext, "pretty_date", pretty_date, gshkl_filter_cleanup);
	gshkl_add_filter(ctext, "thumbnail_for_image", thumbnail_for_image, gshkl_filter_cleanup);

	greshunkel_var posts = gshkl_add_array(ctext, "POSTS");

	PGresult *res = get_posts_by_thread_id(_thread->id);
	unsigned int total_rows = 0;
	if (res) {
		unsigned int i = 0;
		total_rows = PQntuples(res);
		for (i = 0; i < total_rows; i++) {
			post *dsrlzd = deserialize_post_from_tuples(res, i);

			if (dsrlzd) {
				greshunkel_ctext *_post_sub = gshkl_init_context();
				gshkl_add_int(_post_sub, "date", dsrlzd->fourchan_post_id);
				gshkl_add_string(_post_sub, "board", dsrlzd->board);

				if (dsrlzd->body_content)
					gshkl_add_string(_post_sub, "content", dsrlzd->body_content);
				else
					gshkl_add_string(_post_sub, "content", "");

				if (dsrlzd->fourchan_post_no)
					gshkl_add_int(_post_sub, "post_no", dsrlzd->fourchan_post_no);
				else
					gshkl_add_string(_post_sub, "post_no", "");

				const char *w_filename = PQgetvalue(res, i, PQfnumber(res, "w_filename"));
				const char *wa_filename = PQgetvalue(res, i, PQfnumber(res, "wa_filename"));
				if (w_filename && strlen(w_filename)) {
					gshkl_add_string(_post_sub, "image", w_filename);
				} else if (wa_filename && strlen(wa_filename)) {
					gshkl_add_string(_post_sub, "image", wa_filename);
				} else {
					gshkl_add_string(_post_sub, "image", NULL);
				}

				gshkl_add_sub_context_to_loop(&posts, _post_sub);

				vector_free(dsrlzd->replied_to_keys);
				free(dsrlzd->body_content);
			}

			free(dsrlzd);
		}
	}

	PQclear(res);

	free(_thread);

	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());

	vector_reverse(posts.arr);

	gshkl_add_int(ctext, "total", total_rows);

	return m38_render_file(ctext, "./templates/by_thread.html", response);
}

int by_alias_handler(const m38_http_request *request, m38_http_response *response) {
	const unsigned int page = strtol(request->resource + request->matches[1].rm_so, NULL, 10);

	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_filter(ctext, "thumbnail_for_image", thumbnail_for_image, gshkl_filter_cleanup);
	greshunkel_var images = gshkl_add_array(ctext, "IMAGES");
	int total = _add_sorted_by_aliases(&images, OFFSET_FOR_PAGE(page), RESULTS_PER_PAGE);
	if (total == 0) {
		gshkl_add_string_to_loop(&images, "None");
	}

	greshunkel_var pages = gshkl_add_array(ctext, "PAGES");
	unsigned int i;
	const unsigned int max = total/RESULTS_PER_PAGE;
	for (i = 0; i < max; i++)
		gshkl_add_int_to_loop(&pages, i);

	if (page > 0) {
		gshkl_add_int(ctext, "prev_page", page - 1);
	} else {
		gshkl_add_string(ctext, "prev_page", "");
	}

	if (page < max) {
		gshkl_add_int(ctext, "next_page", page + 1);
	} else {
		gshkl_add_string(ctext, "next_page", "");
	}

	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());

	gshkl_add_int(ctext, "total", total);
	return m38_render_file(ctext, "./templates/no_board.html", response);
}

int board_handler(const m38_http_request *request, m38_http_response *response) {
	return _board_handler(request, response, 0);
}

int paged_board_handler(const m38_http_request *request, m38_http_response *response) {
	const unsigned int page = strtol(request->resource + request->matches[2].rm_so, NULL, 10);
	return _board_handler(request, response, page);
}

int favicon_handler(const m38_http_request *request, m38_http_response *response) {
	UNUSED(request);
	return m38_mmap_file("./static/favicon.ico", response);
}

int robots_handler(const m38_http_request *request, m38_http_response *response) {
	UNUSED(request);
	return m38_mmap_file("./static/robots.txt", response);
}

int api_index_stats(const m38_http_request *request, m38_http_response *response) {
	UNUSED(request);
	char *out = NULL;

	PGresult *webmr = get_api_index_state_webms();
	PGresult *webm_aliasr = get_api_index_state_aliases();
	PGresult *postsr = get_api_index_state_posts();

	JSON_Value *root_value = json_value_init_object();
	JSON_Object *root_object = json_value_get_object(root_value);

	JSON_Value *_webm_arr = json_value_init_array();
	JSON_Array *webm_arr = json_value_get_array(_webm_arr);
	JSON_Value *_webm_alias_arr = json_value_init_array();
	JSON_Array *webm_alias_arr = json_value_get_array(_webm_alias_arr);
	JSON_Value *_posts_arr = json_value_init_array();
	JSON_Array *posts_arr = json_value_get_array(_posts_arr);

	int i = 0;
	for (i = 0; i < PQntuples(webmr); i++) {
		JSON_Value *data_point = json_value_init_object();
		JSON_Object *obj = json_value_get_object(data_point);

		json_object_set_string(obj, "x", PQgetvalue(webmr, i, 0));
		json_object_set_number(obj, "y", atol(PQgetvalue(webmr, i, 1)));

		json_array_append_value(webm_arr, data_point);
	}

	for (i = 0; i < PQntuples(webm_aliasr); i++) {
		JSON_Value *data_point = json_value_init_object();
		JSON_Object *obj = json_value_get_object(data_point);

		json_object_set_string(obj, "x", PQgetvalue(webm_aliasr, i, 0));
		json_object_set_number(obj, "y", atol(PQgetvalue(webm_aliasr, i, 1)));

		json_array_append_value(webm_alias_arr, data_point);
	}

	for (i = 0; i < PQntuples(postsr); i++) {
		JSON_Value *data_point = json_value_init_object();
		JSON_Object *obj = json_value_get_object(data_point);

		json_object_set_string(obj, "x", PQgetvalue(postsr, i, 0));
		json_object_set_number(obj, "y", atol(PQgetvalue(postsr, i, 1)));

		json_array_append_value(posts_arr, data_point);
	}

	json_object_set_value(root_object, "webm_data", _webm_arr);
	json_object_set_value(root_object, "alias_data", _webm_alias_arr);
	json_object_set_value(root_object, "posts_data", _posts_arr);

	out = json_serialize_to_string(root_value);

	PQclear(webmr);
	PQclear(webm_aliasr);
	PQclear(postsr);

	return m38_return_raw_buffer(out, strlen(out), response);
}

int admin_index_handler(const m38_http_request *request, m38_http_response *response) {
	UNUSED(request);
	greshunkel_ctext *ctext = gshkl_init_context();
	gshkl_add_int(ctext, "webm_count", webm_count());
	gshkl_add_int(ctext, "alias_count", webm_alias_count());
	gshkl_add_int(ctext, "post_count", post_count());

	greshunkel_var boards = gshkl_add_array(ctext, "BOARDS");
	_add_files_in_dir_to_arr(&boards, webm_location());
	return m38_render_file(ctext, "./templates/admin/index.html", response);
}
