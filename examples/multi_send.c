#include "evpath.h"

typedef struct _simple_rec {
    int integer_field;
} simple_rec, *simple_rec_ptr;

static IOField simple_field_list[] =
{
    {"integer_field", "integer", sizeof(int), IOOffset(simple_rec_ptr, integer_field)},
    {NULL, NULL, 0, 0}
};
static CMFormatRec simple_format_list[] =
{
    {"simple", simple_field_list},
    {NULL, NULL}
};

/* this file is evpath/examples/multi_send.c */
int main(int argc, char **argv)
{
    CManager cm;
    simple_rec data;
    EVstone split_stone;
    EVaction split_action;
    EVsource source;
    int i;

    cm = CManager_create();
    CMlisten(cm);

    split_stone = EValloc_stone(cm);
    split_action = EVassoc_split_action(cm, split_stone, NULL);

    for (i = 1; i < argc; i++) {
	char string_list[2048];
	attr_list contact_list;
	EVstone remote_stone, output_stone;
        if (sscanf(argv[1], "%d:%s", &remote_stone, &string_list[0]) != 2) {
	    printf("Bad argument \"%s\"\n", argv[1]);
	    exit(0);
	}
	output_stone = EValloc_stone(cm);
	contact_list = attr_list_from_string(string_list);
	EVassoc_output_action(cm, output_stone, contact_list, remote_stone);
	EVaction_add_split_target(cm, split_stone, split_action, output_stone);
    }

    source = EVcreate_submit_handle(cm, split_stone, simple_format_list);
    data.integer_field = 318;
    EVsubmit(source, &data, NULL);
}