// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "muse_options.h"

static cJSON *fixture(void)
{
    return cJSON_Parse("{\"message_id\":\"assistant-1\",\"display_text\":\"Which meal?\",\"widgets\":["
        "{\"widget_id\":\"meal-1\",\"kind\":\"option\",\"data\":{\"options\":[\"Pasta\",\"寿司\",\"Salad\"]},"
        "\"state\":{\"version\":3,\"data\":{}}}]}");
}
static cJSON *widget(cJSON *root) { return cJSON_GetArrayItem(cJSON_GetObjectItem(root, "widgets"), 0); }
static void receive(cJSON *root)
{
    assert(root && muse_options_receive(root));
}
static muse_options_t view(void)
{
    muse_options_t q;
    assert(muse_options_snapshot(&q));
    return q;
}
static void basic(void)
{
    cJSON *root = fixture();
    receive(root);
    muse_options_t q = view(), taken;
    assert(q.count == 3 && q.selected == -1 && q.state_version == 3);
    assert(!strcmp(q.labels[1], "寿司") && !strcmp(q.question, "Which meal?"));
    assert(!muse_options_request(q.generation, -1) && !muse_options_request(q.generation, 3));
    assert(muse_options_request(q.generation, 1));
    assert(!muse_options_request(q.generation, 0));
    receive(root); /* duplicate full row cannot reopen an answer */
    assert(muse_options_pending() && muse_options_take(&taken) && !muse_options_take(&taken));
    assert(taken.selected == 1);
    cJSON *body = muse_options_chat_body(&taken, "interaction-test");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "message")), "寿司"));
    cJSON *origin = cJSON_GetObjectItem(cJSON_GetObjectItem(body, "metadata"), "message_origin");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(origin, "widget_id")), "meal-1"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(origin, "message_id")), "assistant-1"));
    assert(cJSON_GetObjectItem(origin, "option_index")->valueint == 1);
    cJSON_Delete(body);
    body = muse_options_state_body(&taken, "mutation-test");
    assert(cJSON_GetObjectItem(body, "base_version")->valueint == 3);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(body, "state"), "selectedText")), "寿司"));
    cJSON_Delete(body);
    muse_options_result(q.generation, MUSE_OPTIONS_SELECTED, false, "Save failed");
    receive(root);
    assert(view().status == MUSE_OPTIONS_SELECTED && !view().saved);
    assert(!muse_options_request(q.generation, 1));
    cJSON_Delete(root);
}
static void invalid(int mode)
{
    cJSON *root = fixture(), *data = cJSON_GetObjectItem(widget(root), "data");
    if (mode == 1) cJSON_ReplaceItemInObject(data, "options", cJSON_Parse("[]"));
    if (mode == 2) cJSON_ReplaceItemInObject(data, "options", cJSON_Parse("[\"a\",\"a\"]"));
    if (mode == 3) cJSON_ReplaceItemInObject(data, "options", cJSON_Parse("[1,2]"));
    if (mode == 4) {
        char large[MUSE_OPTION_BYTES + 10]; memset(large, 'x', sizeof(large)); large[sizeof(large)-1]=0;
        cJSON_ReplaceItemInArray(cJSON_GetObjectItem(data, "options"), 0, cJSON_CreateString(large));
    }
    if (mode == 5) cJSON_AddItemToObject(data, "option_actions", cJSON_Parse("[{\"index\":0,\"url\":\"https://example.com\"}]"));
    if (mode == 6) cJSON_ReplaceItemInObject(data, "options", cJSON_Parse("[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]"));
    if (mode == 7) cJSON_DeleteItemFromObject(root, "message_id");
    if (mode == 8) cJSON_ReplaceItemInObject(data, "options", cJSON_Parse("[\"\"]"));
    assert(!muse_options_receive(root));
    muse_options_t q; assert(!muse_options_snapshot(&q));
    cJSON_Delete(root);
}
static void generations(void)
{
    cJSON *root=fixture(); receive(root); muse_options_t old=view();
    assert(muse_options_request(old.generation, 0));
    cJSON_ReplaceItemInObject(widget(root), "widget_id", cJSON_CreateString("new-question"));
    receive(root); muse_options_t current=view();
    assert(current.generation != old.generation && !muse_options_pending());
    assert(!muse_options_request(old.generation, 1));
    muse_options_result(old.generation, MUSE_OPTIONS_SELECTED, true, "");
    assert(view().status==MUSE_OPTIONS_READY);
    muse_options_clear(); assert(!muse_options_snapshot(&current));
    cJSON_Delete(root);
}
static void disconnects(void)
{
    cJSON *root=fixture(); receive(root); muse_options_t q=view();
    assert(muse_options_request(q.generation,0)); assert(muse_options_take(&q));
    muse_options_disconnected(); receive(root);
    assert(view().status==MUSE_OPTIONS_UNCERTAIN && !muse_options_request(q.generation,1));
    muse_options_clear(); receive(root); muse_options_disconnected();
    assert(view().status==MUSE_OPTIONS_STALE);
    cJSON_Delete(root);
}
static void phone(void)
{
    cJSON *root=fixture(); receive(root); muse_options_t q=view();
    assert(muse_options_request(q.generation,0)); assert(muse_options_take(&q));
    cJSON *state=cJSON_GetObjectItem(widget(root),"state");
    cJSON_AddStringToObject(cJSON_GetObjectItem(state,"data"),"selectedText","Salad");
    cJSON_ReplaceItemInObject(state,"version",cJSON_CreateNumber(4)); receive(root);
    assert(view().selected==2 && view().saved);
    muse_options_result(q.generation,MUSE_OPTIONS_UNCERTAIN,false,"lost ack");
    assert(view().status==MUSE_OPTIONS_SELECTED && view().selected==2);
    cJSON *response=cJSON_Parse("{\"applied\":false,\"state\":{\"version\":4,\"data\":{\"selectedText\":\"Salad\"}}}");
    assert(!muse_options_state_response(&q,response));
    q.selected=2; assert(muse_options_state_response(&q,response));
    cJSON_Delete(response); cJSON_Delete(root);
}
static void encoded(void)
{
    cJSON *root=fixture(), *w=widget(root);
    char *data=cJSON_PrintUnformatted(cJSON_GetObjectItem(w,"data"));
    cJSON_ReplaceItemInObject(w,"data",cJSON_CreateString(data)); cJSON_free(data);
    cJSON_ReplaceItemInObject(w,"state",cJSON_CreateString("{\"version\":2,\"data\":{\"selectedText\":\"Pasta\"}}"));
    receive(root); assert(view().selected==0 && view().saved);
    cJSON_Delete(root);
}
static void state_events(void)
{
    cJSON *root=fixture(); receive(root);
    assert(muse_options_waiting());
    cJSON *event=cJSON_Parse("{\"host_message_id\":\"assistant-1\",\"widget_id\":\"meal-1\",\"state\":{\"version\":4,\"data\":{\"selectedText\":\"Salad\"}}}");
    assert(muse_options_receive(event));
    assert(view().selected==2 && view().saved && !muse_options_waiting());
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(event,"state"),"version",cJSON_CreateNumber(2));
    assert(!muse_options_receive(event));
    assert(view().selected==2 && view().state_version==4);
    cJSON_Delete(event); cJSON_Delete(root);
}
static void question_text(void)
{
    cJSON *root=fixture();
    cJSON_DeleteItemFromObject(root,"display_text");
    receive(root); muse_options_t initial=view();
    assert(!initial.question[0]);
    /* A canonical row can arrive after the widget, with content nested in payload. */
    cJSON_AddStringToObject(root,"display_text","");
    cJSON_AddItemToObject(root,"payload",cJSON_Parse("{\"content\":\"What do you want for lunch?\\n\\n[[hatch_widget:meal-1]]\"}"));
    receive(root); muse_options_t updated=view();
    assert(!strcmp(updated.question,"What do you want for lunch?"));
    assert(updated.generation==initial.generation && updated.revision>initial.revision);
    assert(muse_options_request(updated.generation,1));
    cJSON *data=cJSON_GetObjectItem(widget(root),"data");
    cJSON_AddStringToObject(data,"title","");
    receive(root); assert(!strcmp(view().question,updated.question));
    cJSON_AddStringToObject(widget(root),"title","Which lunch?");
    receive(root); updated=view();
    assert(!strcmp(updated.question,"Which lunch?") && updated.status==MUSE_OPTIONS_QUEUED && updated.selected==1);
    cJSON_DeleteItemFromObject(widget(root),"title");
    cJSON_DeleteItemFromObject(root,"payload");
    receive(root); assert(!strcmp(view().question,"Which lunch?"));
    cJSON_Delete(root);
}
static void question_sources(void)
{
    for (int mode=0; mode<4; ++mode) {
        muse_options_clear();
        cJSON *root=fixture(), *data=cJSON_GetObjectItem(widget(root),"data");
        cJSON_ReplaceItemInObject(root,"display_text",cJSON_CreateString(" \n[[hatch_widget:meal-1]]\t"));
        cJSON_AddStringToObject(data,"title"," \t");
        if (mode==0) cJSON_AddStringToObject(root,"content","What do you want for lunch?");
        if (mode==1) cJSON_AddItemToObject(root,"payload",cJSON_Parse("{\"content\":\"What do you want for lunch?\"}"));
        if (mode==2) cJSON_ReplaceItemInObject(data,"title",cJSON_CreateString("What do you want for lunch?"));
        if (mode==3) cJSON_AddStringToObject(widget(root),"title","What do you want for lunch?");
        receive(root);
        assert(!strcmp(view().question,"What do you want for lunch?"));
        cJSON_Delete(root);
    }
    cJSON *root=fixture();
    char long_question[MUSE_OPTION_QUESTION_BYTES*2];
    for (size_t i=0; i<sizeof(long_question)-3; i+=3) memcpy(long_question+i,"寿",3);
    long_question[sizeof(long_question)-3]='\0';
    cJSON_ReplaceItemInObject(root,"display_text",cJSON_CreateString(long_question));
    receive(root); muse_options_t q=view();
    assert(strlen(q.question)==(MUSE_OPTION_QUESTION_BYTES-1)/3*3);
    assert(!strncmp(q.question,long_question,strlen(q.question)));
    cJSON_Delete(root);
}
static void transcript_question(void)
{
    cJSON *root=fixture();
    cJSON_DeleteItemFromObject(root,"display_text");
    /* Current Muse canonical rows carry assistant text in transcript blocks. */
    cJSON_AddItemToObject(root,"transcript",cJSON_Parse("{\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Do not show user text\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"image\",\"text\":\"Not text\"},"
        "{\"type\":\"text\",\"text\":\"What do you want for lunch?\"},"
        "{\"type\":\"text\",\"text\":\"[[hatch_widget:meal-1]]\"}]}]}"));
    receive(root); assert(!strcmp(view().question,"What do you want for lunch?"));
    cJSON_Delete(root);
}
int main(int argc,char **argv)
{
    assert(argc==2); int mode=atoi(argv[1]);
    if (mode==0) basic();
    else if (mode<=8) invalid(mode);
    else if (mode==9) generations();
    else if (mode==10) disconnects();
    else if (mode==11) phone();
    else if (mode==12) encoded();
    else if (mode==13) state_events();
    else if (mode==14) question_text();
    else if (mode==15) question_sources();
    else if (mode==16) transcript_question();
    else return 2;
    return 0;
}
