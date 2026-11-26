/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* The flat JSON reader and writer (src/mg_json.c). */

#include <errno.h>

#include "check.h"
#include "mg_json.h"

static int parse(const char *text, struct mg_json_obj *o)
{
	static char buf[4096];
	size_t n = strlen(text);

	memcpy(buf, text, n);
	buf[n] = 'X'; /* not NUL-terminated: the parser must stop at len */
	return mg_json_parse(buf, n, o);
}

static void test_accepts(void)
{
	static struct mg_json_obj o;
	const struct mg_json_field *f;

	CHECK(parse("{}", &o) == 0 && o.n == 0);
	CHECK(parse(" \t\r\n{ } \n", &o) == 0 && o.n == 0);
	CHECK(parse("{\"action\":\"pairing_client_hello\",\"version\":5,\"x\":-12,\"t\":true,"
		    "\"f\":false,\"n\":null,\"d\":1.5e3,\"z\":0,\"nested\":{\"a\":[1,{\"b\":[]},\"]\"]},"
		    "\"arr\":[]}",
		    &o) == 0);
	CHECK(o.n == 10);
	CHECK_STR(mg_json_str(&o, "action"), "pairing_client_hello");
	f = mg_json_get(&o, "version");
	CHECK(f && f->type == MG_JSON_NUMBER && f->num_is_int && f->num == 5);
	f = mg_json_get(&o, "x");
	CHECK(f && f->num_is_int && f->num == -12);
	CHECK(mg_json_get(&o, "t")->type == MG_JSON_TRUE);
	CHECK(mg_json_get(&o, "f")->type == MG_JSON_FALSE);
	CHECK(mg_json_get(&o, "n")->type == MG_JSON_NULL);
	f = mg_json_get(&o, "d");
	CHECK(f && f->type == MG_JSON_NUMBER && !f->num_is_int);
	CHECK(mg_json_get(&o, "z")->num == 0 && mg_json_get(&o, "z")->num_is_int);
	CHECK(mg_json_get(&o, "nested")->type == MG_JSON_NESTED);
	CHECK(mg_json_get(&o, "arr")->type == MG_JSON_NESTED);
	CHECK(mg_json_str(&o, "version") == NULL); /* not a string */
	CHECK(mg_json_str(&o, "missing") == NULL);

	/* Escapes, decoded in place. */
	CHECK(parse("{\"s\":\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\\u0041\\u00e9\\u20ac\\ud83d\\ude00\","
		    "\"k\\u0065y\":\"\"}",
		    &o) == 0);
	f = mg_json_get(&o, "s");
	CHECK(f && f->str_len == 22 &&
	      memcmp(f->str, "a\"b\\c/d\b\f\n\r\tA\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", f->str_len) ==
		      0);
	f = mg_json_get(&o, "key");
	CHECK(f && f->type == MG_JSON_STRING && f->str_len == 0 && f->str[0] == '\0');
	/* Big integers are numbers, not ints. */
	CHECK(parse("{\"n\":123456789012345678901234567890}", &o) == 0 &&
	      !mg_json_get(&o, "n")->num_is_int);
	CHECK(parse("{\"n\":-0}", &o) == 0 && mg_json_get(&o, "n")->num == 0);
	/* Raw UTF-8 passes through. */
	CHECK(parse("{\"s\":\"\xc3\xbc\"}", &o) == 0 && mg_json_get(&o, "s")->str_len == 2);
}

static void test_refuses(void)
{
	static struct mg_json_obj o;
	static const char *const bad[] = {
		"",
		"[]",
		"\"s\"",
		"5",
		"{",
		"}",
		"{\"a\"}",
		"{\"a\":}",
		"{\"a\":1,}",
		"{,}",
		"{\"a\":1}{",
		"{\"a\":1} x",
		"{a:1}",
		"{'a':1}",
		"{\"a\":01}",
		"{\"a\":1.}",
		"{\"a\":.5}",
		"{\"a\":1e}",
		"{\"a\":-}",
		"{\"a\":+1}",
		"{\"a\":tru}",
		"{\"a\":nul}",
		"{\"a\":\"x}",
		"{\"a\":\"\\x\"}",
		"{\"a\":\"\\u12\"}",
		"{\"a\":\"\\u0000\"}",
		"{\"a\":\"\\ud800\"}",
		"{\"a\":\"\\udc00\"}",
		"{\"a\":\"\\ud800\\u0041\"}",
		"{\"a\":\"tab\there\"}",
		"{\"a\":[1,]}",
		"{\"a\":[1 2]}",
		"{\"a\":{\"b\"}}",
		"{\"a\":{\"b\":1,}}",
		"{\"a\":1,\"a\":2}",
	};

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int err = parse(bad[i], &o);

		if (err == 0) {
			fprintf(stderr, "    accepted: %s\n", bad[i]);
		}
		CHECK(err != 0);
	}
	/* A raw control character inside a string. */
	CHECK(parse("{\"a\":\"x\x01y\"}", &o) != 0);
	/* Embedded NUL in the input. */
	{
		char b[] = "{\"a\":\"x\0y\"}";

		CHECK(mg_json_parse(b, sizeof(b) - 1, &o) != 0);
	}
	/* Depth: 32 levels of nesting are fine, 33 are not. */
	char deep[200];
	size_t n = 0;

	n += (size_t)snprintf(&deep[n], sizeof(deep) - n, "{\"a\":");
	for (int i = 0; i < 31; i++) {
		deep[n++] = '[';
	}
	for (int i = 0; i < 31; i++) {
		deep[n++] = ']';
	}
	deep[n++] = '}';
	deep[n] = 0;
	CHECK(parse(deep, &o) == 0);
	n = (size_t)snprintf(deep, sizeof(deep), "{\"a\":");
	for (int i = 0; i < 32; i++) {
		deep[n++] = '[';
	}
	for (int i = 0; i < 32; i++) {
		deep[n++] = ']';
	}
	deep[n++] = '}';
	deep[n] = 0;
	CHECK(parse(deep, &o) == -E2BIG);
	/* Too many members. */
	char many[1024];

	n = (size_t)snprintf(many, sizeof(many), "{");
	for (int i = 0; i <= MG_JSON_MAX_FIELDS; i++) {
		n += (size_t)snprintf(&many[n], sizeof(many) - n, "%s\"k%d\":%d", i ? "," : "", i, i);
	}
	snprintf(&many[n], sizeof(many) - n, "}");
	CHECK(parse(many, &o) == -E2BIG);
}

static void test_writer(void)
{
	char buf[128];
	struct mg_jw w;

	mg_jw_begin(&w, buf, sizeof(buf));
	mg_jw_str(&w, "type", "status");
	mg_jw_int(&w, "n", -42);
	mg_jw_int(&w, "z", 0);
	mg_jw_str(&w, "e", "q\"b\\c\n\x01");
	CHECK(mg_jw_end(&w) > 0);
	CHECK_STR(buf, "{\"type\":\"status\",\"n\":-42,\"z\":0,\"e\":\"q\\\"b\\\\c\\u000a\\u0001\"}");
	/* Round trip through the reader. */
	static struct mg_json_obj o;

	CHECK(mg_json_parse(buf, strlen(buf), &o) == 0);
	CHECK(mg_json_get(&o, "n")->num == -42);
	CHECK(memcmp(mg_json_str(&o, "e"), "q\"b\\c\n\x01", 7) == 0);
	/* Overflow: -ENOSPC and an empty string. */
	mg_jw_begin(&w, buf, 10);
	mg_jw_str(&w, "type", "status");
	CHECK(mg_jw_end(&w) == -ENOSPC && buf[0] == '\0');
	mg_jw_begin(&w, buf, 3);
	CHECK(mg_jw_end(&w) == 2);
	CHECK_STR(buf, "{}");
	/* Measured only (no buffer): the same length, the same limit. */
	mg_jw_begin(&w, NULL, 64);
	mg_jw_str(&w, "e", "q\"b\\c\n\x01");
	CHECK(mg_jw_end(&w) == (int)strlen("{\"e\":\"q\\\"b\\\\c\\u000a\\u0001\"}"));
	mg_jw_begin(&w, NULL, 10);
	mg_jw_str(&w, "type", "status");
	CHECK(mg_jw_end(&w) == -ENOSPC);
}

/* Nested writing (link.register's commands) and a nested member's raw text
 * (link.invoke's params), which the reader leaves undecoded. */
static void test_nested(void)
{
	char buf[160];
	struct mg_jw w;
	static struct mg_json_obj o, inner;

	mg_jw_begin(&w, buf, sizeof(buf));
	mg_jw_int(&w, "v", 1);
	mg_jw_arr(&w, "a");
	mg_jw_obj(&w, NULL);
	mg_jw_str(&w, "n", "x");
	mg_jw_obj(&w, "req");
	mg_jw_close(&w);
	mg_jw_close(&w);
	mg_jw_int(&w, NULL, 7);
	mg_jw_bool(&w, NULL, false);
	mg_jw_close(&w);
	mg_jw_raw(&w, "p", "{\"r\":1}", 7);
	mg_jw_obj(&w, "m");
	mg_jw_bool(&w, "ok", true); /* closed by mg_jw_end */
	CHECK(mg_jw_end(&w) > 0);
	CHECK_STR(buf, "{\"v\":1,\"a\":[{\"n\":\"x\",\"req\":{}},7,false],\"p\":{\"r\":1},"
		       "\"m\":{\"ok\":true}}");
	/* Closing the outermost object early is an error. */
	mg_jw_begin(&w, buf, sizeof(buf));
	mg_jw_close(&w);
	CHECK(mg_jw_end(&w) == -ENOSPC);

	char text[] = "{\"name\":\"a\\u00e9\",\"params\":{\"s\":\"q\\\"\\u00e9\",\"n\":[1,{\"x\":2}]},"
		      "\"l\":[]}";

	CHECK(mg_json_parse(text, strlen(text), &o) == 0);
	CHECK_STR(mg_json_str(&o, "name"), "a\xc3\xa9");
	const struct mg_json_field *pf = mg_json_get(&o, "params");

	CHECK(pf->type == MG_JSON_NESTED && !pf->is_array);
	CHECK(pf->raw_len == strlen("{\"s\":\"q\\\"\\u00e9\",\"n\":[1,{\"x\":2}]}"));
	CHECK(memcmp(pf->raw, "{\"s\":\"q\\\"\\u00e9\",\"n\":[1,{\"x\":2}]}", pf->raw_len) == 0);
	CHECK(mg_json_get(&o, "l")->is_array && mg_json_get(&o, "l")->raw_len == 2);
	/* The raw text parses in turn, decoding then. */
	CHECK(mg_json_parse(pf->raw, pf->raw_len, &inner) == 0);
	CHECK_STR(mg_json_str(&inner, "s"), "q\"\xc3\xa9");
	CHECK(mg_json_get(&inner, "n")->is_array);
	/* Nested strings are still checked. */
	char bad[] = "{\"p\":{\"s\":\"\\uD800\"}}";

	CHECK(mg_json_parse(bad, strlen(bad), &o) == -EINVAL);
}

HOST_MAIN(test_accepts, test_refuses, test_writer, test_nested)
