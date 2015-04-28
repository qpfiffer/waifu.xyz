// vim: noet ts=4 sw=4
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <oleg-http/oleg-http.h>
#include <38-moths/logging.h>

#include "db.h"
#include "models.h"
#include "parson.h"
#include "utils.h"

void create_webm_key(const char file_hash[static HASH_IMAGE_STR_SIZE], char outbuf[static MAX_KEY_SIZE]) {
	snprintf(outbuf, MAX_KEY_SIZE, "%s%s", WEBM_NMSPC, file_hash);
}

webm *deserialize_webm(const char *json) {
	if (!json)
		return NULL;

	webm *to_return = calloc(1, sizeof(webm));

	JSON_Value *serialized = json_parse_string(json);
	JSON_Object *webm_object = json_value_get_object(serialized);

	strncpy(to_return->file_hash, json_object_get_string(webm_object, "file_hash"), sizeof(to_return->file_hash));
	strncpy(to_return->filename, json_object_get_string(webm_object, "filename"), sizeof(to_return->filename));
	strncpy(to_return->board, json_object_get_string(webm_object, "board"), sizeof(to_return->board));
	strncpy(to_return->file_path, json_object_get_string(webm_object, "file_path"), sizeof(to_return->file_path));

	const char *post = json_object_get_string(webm_object, "post");
	if (post != NULL)
		strncpy(to_return->post, post, sizeof(to_return->post));


	to_return->created_at = (time_t)json_object_get_number(webm_object, "created_at");
	to_return->size = (size_t)json_object_get_number(webm_object, "size");

	json_value_free(serialized);
	return to_return;
}

char *serialize_webm(const webm *to_serialize) {
	if (!to_serialize)
		return NULL;

	JSON_Value *root_value = json_value_init_object();
	JSON_Object *root_object = json_value_get_object(root_value);

	char *serialized_string = NULL;

	/* json_object_set_boolean(root_object, "is_alias", (int)to_serialize->is_alias); */

	json_object_set_string(root_object, "file_hash", to_serialize->file_hash);
	json_object_set_string(root_object, "filename", to_serialize->filename);
	json_object_set_string(root_object, "board", to_serialize->board);
	json_object_set_string(root_object, "file_path", to_serialize->file_path);

	if (to_serialize->post)
		json_object_set_string(root_object, "post", to_serialize->post);

	json_object_set_number(root_object, "created_at", to_serialize->created_at);
	json_object_set_number(root_object, "size", to_serialize->size);

	serialized_string = json_serialize_to_string(root_value);
	json_value_free(root_value);
	return serialized_string;
}

static unsigned int x_count(const char prefix[static MAX_KEY_SIZE]) {
	unsigned int num = fetch_num_matches_from_db(&oleg_conn, prefix);
	return num;
}

unsigned int webm_count() {
	char prefix[MAX_KEY_SIZE] = WEBM_NMSPC;
	return x_count(prefix);
}

unsigned int webm_alias_count() {
	char prefix[MAX_KEY_SIZE] = ALIAS_NMSPC;
	return x_count(prefix);
}

unsigned int post_count() {
	char prefix[MAX_KEY_SIZE] = POST_NMSPC;
	return x_count(prefix);
}

void create_alias_key(const char file_path[static MAX_IMAGE_FILENAME_SIZE], char outbuf[static MAX_KEY_SIZE]) {
	/* MORE HASHES IS MORE POWER */
	char str_hash[HASH_IMAGE_STR_SIZE] = {0};
	hash_string((unsigned char *)file_path, strnlen(file_path, MAX_IMAGE_FILENAME_SIZE), str_hash);

	char second_hash[HASH_IMAGE_STR_SIZE] = {0};
	hash_string_fnv1a((unsigned char *)file_path, strnlen(file_path, MAX_IMAGE_FILENAME_SIZE), second_hash);

	snprintf(outbuf, MAX_KEY_SIZE, "%s%s%s", ALIAS_NMSPC, str_hash, second_hash);
}

char *serialize_alias(const webm_alias *to_serialize) {
	if (!to_serialize)
		return NULL;

	JSON_Value *root_value = json_value_init_object();
	JSON_Object *root_object = json_value_get_object(root_value);

	char *serialized_string = NULL;

	json_object_set_string(root_object, "file_hash", to_serialize->file_hash);
	json_object_set_string(root_object, "filename", to_serialize->filename);
	json_object_set_string(root_object, "board", to_serialize->board);
	json_object_set_string(root_object, "file_path", to_serialize->file_path);

	if (to_serialize->post)
		json_object_set_string(root_object, "post", to_serialize->post);

	json_object_set_number(root_object, "created_at", to_serialize->created_at);

	serialized_string = json_serialize_to_string(root_value);

	json_value_free(root_value);
	return serialized_string;
}

webm_alias *deserialize_alias(const char *json) {
	if (!json)
		return NULL;

	webm_alias *to_return = calloc(1, sizeof(webm_alias));

	JSON_Value *serialized = json_parse_string(json);
	JSON_Object *webm_alias_object = json_value_get_object(serialized);

	strncpy(to_return->file_hash, json_object_get_string(webm_alias_object, "file_hash"), sizeof(to_return->file_hash));
	strncpy(to_return->filename, json_object_get_string(webm_alias_object, "filename"), sizeof(to_return->filename));
	strncpy(to_return->board, json_object_get_string(webm_alias_object, "board"), sizeof(to_return->board));
	strncpy(to_return->file_path, json_object_get_string(webm_alias_object, "file_path"), sizeof(to_return->file_path));

	const char *post = json_object_get_string(webm_alias_object, "post");
	if (post != NULL)
		strncpy(to_return->post, post, sizeof(to_return->post));

	to_return->created_at = (time_t)json_object_get_number(webm_alias_object, "created_at");

	json_value_free(serialized);
	return to_return;
}

void create_webm_to_alias_key(const char file_hash[static HASH_IMAGE_STR_SIZE], char outbuf[static MAX_KEY_SIZE]) {
	snprintf(outbuf, MAX_KEY_SIZE, "%s%s", WEBMTOALIAS_NMSPC, file_hash);
}

char *serialize_webm_to_alias(const webm_to_alias *w2a) {
	if (!w2a)
		return NULL;

	JSON_Value *root_value = json_value_init_array();
	JSON_Array *root_array = json_value_get_array(root_value);

	char *serialized_string = NULL;

	unsigned int i;
	for (i = 0; i < w2a->aliases->count; i++) {
		json_array_append_string(root_array, vector_get(w2a->aliases, i));
	}

	serialized_string = json_serialize_to_string(root_value);

	json_value_free(root_value);
	return serialized_string;
}

webm_to_alias *deserialize_webm_to_alias(const char *json) {
	if (!json)
		return NULL;

	webm_to_alias *to_return = calloc(1, sizeof(webm_to_alias));

	JSON_Value *serialized = json_parse_string(json);
	JSON_Array *webm_to_alias_object = json_value_get_array(serialized);

	const size_t num_aliases  = json_array_get_count(webm_to_alias_object);

	to_return->aliases = vector_new(MAX_KEY_SIZE, num_aliases);

	unsigned int i;
	for (i = 0; i < num_aliases; i++) {
		const char *alias = json_array_get_string(webm_to_alias_object, i);
		vector_append(to_return->aliases, alias, strlen(alias));
	}

	json_value_free(serialized);
	return to_return;
}

void create_thread_key(const char board[static MAX_BOARD_NAME_SIZE], const char *thread_id,
		char outbuf[static MAX_KEY_SIZE]) {
	snprintf(outbuf, MAX_KEY_SIZE, "%s%s%s", THREAD_NMSPC, board, thread_id);
}

char *serialize_thread(const thread *to_serialize) {
	if (!to_serialize)
		return NULL;

	JSON_Value *root_value = json_value_init_object();
	JSON_Object *root_object = json_value_get_object(root_value);

	char *serialized_string = NULL;

	json_object_set_string(root_object, "board", to_serialize->board);

	JSON_Value *post_keys = json_value_init_array();
	JSON_Array *post_keys_array = json_value_get_array(post_keys);

	unsigned int i;
	for (i = 0; i < to_serialize->post_keys->count; i++)
		json_array_append_string(post_keys_array, vector_get(to_serialize->post_keys, i));

	json_object_set_value(root_object, "post_keys", post_keys);

	serialized_string = json_serialize_to_string(root_value);

	json_value_free(root_value);
	return serialized_string;
}

thread *deserialize_thread(const char *json) {
	if (!json)
		return NULL;

	thread *to_return = calloc(1, sizeof(thread));

	JSON_Value *serialized = json_parse_string(json);
	JSON_Object *thread_object = json_value_get_object(serialized);

	strncpy(to_return->board, json_object_get_string(thread_object, "board"), sizeof(to_return->board));

	JSON_Array *post_keys_array = json_object_get_array(thread_object, "post_keys");

	const size_t num_post_keys  = json_array_get_count(post_keys_array);
	to_return->post_keys = vector_new(MAX_KEY_SIZE, num_post_keys);

	unsigned int i;
	for (i = 0; i < num_post_keys; i++) {
		const char *key = json_array_get_string(post_keys_array, i);
		vector_append(to_return->post_keys, key, strlen(key));
	}

	json_value_free(serialized);
	return to_return;
}

void create_post_key(const char board[static MAX_BOARD_NAME_SIZE], const char *post_id,
	char outbuf[static MAX_KEY_SIZE]) {
	snprintf(outbuf, MAX_KEY_SIZE, "%s%s%s", POST_NMSPC, board, post_id);
}

char *serialize_post(const post *to_serialize) {
	if (!to_serialize)
		return NULL;

	JSON_Value *root_value = json_value_init_object();
	JSON_Object *root_object = json_value_get_object(root_value);

	char *serialized_string = NULL;

	json_object_set_string(root_object, "post_id", to_serialize->post_id);
	json_object_set_string(root_object, "thread_key", to_serialize->thread_key);
	json_object_set_string(root_object, "board", to_serialize->board);

	if (to_serialize->webm_key)
		json_object_set_string(root_object, "webm_key", to_serialize->webm_key);

	if (to_serialize->post_no)
		json_object_set_string(root_object, "post_no", to_serialize->post_no);

	if (to_serialize->body_content)
		json_object_set_string(root_object, "body_content", to_serialize->body_content);

	JSON_Value *thread_keys = json_value_init_array();
	JSON_Array *thread_keys_array = json_value_get_array(thread_keys);

	unsigned int i;
	for (i = 0; i < to_serialize->replied_to_keys->count; i++)
		json_array_append_string(thread_keys_array,
				vector_get(to_serialize->replied_to_keys, i));

	json_object_set_value(root_object, "replied_to_keys", thread_keys);

	serialized_string = json_serialize_to_string(root_value);

	json_value_free(root_value);
	return serialized_string;
}

post *deserialize_post(const char *json) {
	if (!json)
		return NULL;

	post *to_return = calloc(1, sizeof(post));

	JSON_Value *serialized = json_parse_string(json);
	JSON_Object *post_object = json_value_get_object(serialized);

	strncpy(to_return->post_id, json_object_get_string(post_object, "post_id"), sizeof(to_return->post_id));
	strncpy(to_return->thread_key, json_object_get_string(post_object, "thread_key"), sizeof(to_return->thread_key));
	strncpy(to_return->board, json_object_get_string(post_object, "board"), sizeof(to_return->board));

	const char *wk = json_object_get_string(post_object, "webm_key");
	if (wk)
		strncpy(to_return->webm_key, json_object_get_string(post_object, "webm_key"), sizeof(to_return->webm_key));

	const char *p_no = json_object_get_string(post_object, "post_no");
	if (p_no)
		strncpy(to_return->post_no, json_object_get_string(post_object, "post_no"), sizeof(to_return->post_no));

	const char *b_content = json_object_get_string(post_object, "body_content");
	if (b_content) {
		to_return->body_content = strdup(b_content);
	} else {
		to_return->body_content = NULL;
	}

	JSON_Array *post_keys_array = json_object_get_array(post_object, "replied_to_keys");

	const size_t num_keys  = json_array_get_count(post_keys_array);
	to_return->replied_to_keys = vector_new(MAX_KEY_SIZE, num_keys);

	unsigned int i;
	for (i = 0; i < num_keys; i++) {
		const char *key = json_array_get_string(post_keys_array, i);
		vector_append(to_return->replied_to_keys, key, strlen(key));
	}

	json_value_free(serialized);
	return to_return;
}
