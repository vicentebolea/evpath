#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <strings.h>

#include "evpath.h"
#include "cm_internal.h"

static IOFormat register_format_set(CManager cm, CMFormatList list, 
				    IOContext *context_ptr);
static void reference_event(event_item *event);
static void return_event(event_path_data evp, event_item *event);
static event_item *get_free_event(event_path_data evp);
static void dump_action(stone_type stone, int a, const char *indent);
extern void print_server_ID(char *server_id);

static const char *action_str[] = { "Action_Output", "Action_Terminal", "Action_Filter", "Action_Decode", "Action_Split"};

void
EVPSubmit_encoded(CManager cm, int local_path_id, void *data, int len)
{
    /* build data record for event and enter it into queue for path */
    /* apply actions to data until none remain (no writes) */
    /* do writes, dereferencing data as necessary */
    /* check data record to see if it is still referenced and act accordingly*/
}

void
EVPSubmit_general(CManager cm, int local_path_id, event_item *event)
{
    
}

EVstone
EValloc_stone(CManager cm)
{
    event_path_data evp = cm->evp;
    int stone_num = evp->stone_count++;
    stone_type stone;

    evp->stone_map = realloc(evp->stone_map, 
			     (evp->stone_count * sizeof(evp->stone_map[0])));
    stone = &evp->stone_map[stone_num];
    memset(stone, 0, sizeof(*stone));
    stone->local_id = stone_num;
    stone->default_action = -1;
    return stone_num;
}

void
EVfree_stone(CManager cm, EVstone stone_num)
{
    event_path_data evp = cm->evp;
    stone_type stone;

    stone = &evp->stone_map[stone_num];
    if (stone->periodic_handle != NULL) {
	CMremove_task(stone->periodic_handle);
	stone->periodic_handle = NULL;
    }
    stone->local_id = -1;
}

EVstone
EVassoc_terminal_action(CManager cm, EVstone stone_num, 
			CMFormatList format_list, EVSimpleHandlerFunc handler,
			void *client_data)
{
    event_path_data evp = cm->evp;
    int action_num;
    stone_type stone = &evp->stone_map[stone_num];
    int proto_action_num = stone->proto_action_count++;
    stone->proto_actions = realloc(stone->proto_actions, 
				   (proto_action_num + 1) * 
				   sizeof(stone->proto_actions[0]));
    memset(&stone->proto_actions[proto_action_num], 0, 
	   sizeof(stone->proto_actions[0]));
    stone->proto_actions[proto_action_num].input_format_requirements =
	format_list;
    stone->proto_actions[proto_action_num].action_type = Action_Terminal;
    stone->proto_actions[proto_action_num].t.term.handler = handler;
    stone->proto_actions[proto_action_num].t.term.client_data = client_data;
    stone->proto_actions[proto_action_num].reference_format = NULL;
    if (format_list != NULL) {
	stone->proto_actions[proto_action_num].reference_format = 
	    register_format_set(cm, format_list, NULL);
    }	
    action_num = stone->action_count++;
    stone->actions = realloc(stone->actions, (action_num + 1) * 
				   sizeof(stone->actions[0]));
    memset(&stone->actions[action_num], 0, sizeof(stone->actions[0]));
    stone->actions[action_num].action_type = Action_Terminal;
    stone->actions[action_num].requires_decoded = 1;
    stone->actions[action_num].reference_format = 
	stone->proto_actions[proto_action_num].reference_format;
    stone->actions[action_num].o.terminal_proto_action_number = proto_action_num;
    return action_num;
}
    

EVstone
EVassoc_filter_action(CManager cm, EVstone stone_num, 
		      CMFormatList format_list, EVSimpleHandlerFunc handler,
		      EVstone out_stone_num, void *client_data)
{
    event_path_data evp = cm->evp;
    int action_num;
    stone_type stone = &evp->stone_map[stone_num];
    int proto_action_num = stone->proto_action_count++;
    stone->proto_actions = realloc(stone->proto_actions, 
				   (proto_action_num + 1) * 
				   sizeof(stone->proto_actions[0]));
    memset(&stone->proto_actions[proto_action_num], 0, 
	   sizeof(stone->proto_actions[0]));
    stone->proto_actions[proto_action_num].input_format_requirements =
	format_list;
    stone->proto_actions[proto_action_num].action_type = Action_Filter;
    stone->proto_actions[proto_action_num].t.term.handler = handler;
    stone->proto_actions[proto_action_num].t.term.client_data = client_data;
    stone->proto_actions[proto_action_num].t.term.target_stone_id = out_stone_num;
    stone->proto_actions[proto_action_num].reference_format = NULL;
    if (format_list != NULL) {
	stone->proto_actions[proto_action_num].reference_format = 
	    register_format_set(cm, format_list, NULL);
    }	
    action_num = stone->action_count++;
    stone->actions = realloc(stone->actions, (action_num + 1) * 
				   sizeof(stone->actions[0]));
    memset(&stone->actions[action_num], 0, sizeof(stone->actions[0]));
    stone->actions[action_num].action_type = Action_Filter;
    stone->actions[action_num].requires_decoded = 1;
    stone->actions[action_num].reference_format = 
	stone->proto_actions[proto_action_num].reference_format;
    stone->actions[action_num].o.terminal_proto_action_number = proto_action_num;
    return action_num;
}
    

static int evpath_locked(){return 1;}

static void
enqueue_event(CManager cm, action *act, event_item *event)
{
    event_path_data evp = cm->evp;
    queue_item *item;
    if (evp->queue_items_free_list == NULL) {
	item = malloc(sizeof(*item));
    } else {
	item = evp->queue_items_free_list;
	evp->queue_items_free_list = item->next;
    }
    item->item = event;
    reference_event(event);
    if (act->queue_head == NULL) {
	act->queue_head = item;
	act->queue_tail = item;
	item->next = NULL;
    } else {
	act->queue_tail->next = item;
	act->queue_tail = item;
	item->next = NULL;
    }
}

static event_item *
dequeue_event(CManager cm, action *act)
{
    event_path_data evp = cm->evp;
    queue_item *item = act->queue_head;
    event_item *event = NULL;
    if (item == NULL) return event;
    event = item->item;
    if (act->queue_head == act->queue_tail) {
	act->queue_head = NULL;
	act->queue_tail = NULL;
    } else {
	act->queue_head = act->queue_head->next;
    }
    item->next = evp->queue_items_free_list;
    evp->queue_items_free_list = item;;
    return event;
}

static void
set_conversions(IOContext ctx, IOFormat src_format, IOFormat target_format)
{
    IOFormat* subformat_list, *saved_subformat_list, *target_subformat_list;
    subformat_list = get_subformats_IOformat(src_format);
    saved_subformat_list = subformat_list;
    target_subformat_list = get_subformats_IOformat(target_format);
    while((subformat_list != NULL) && (*subformat_list != NULL)) {
	char *subformat_name = name_of_IOformat(*subformat_list);
	int j = 0;
	while(target_subformat_list && 
	      (target_subformat_list[j] != NULL)) {
	    if (strcmp(name_of_IOformat(target_subformat_list[j]), 
		       subformat_name) == 0) {
		IOFieldList sub_field_list;
		int sub_struct_size;
		sub_field_list = 
		    field_list_of_IOformat(target_subformat_list[j]);
		sub_struct_size = struct_size_field_list(sub_field_list, 
							 sizeof(char*));
		set_conversion_IOcontext(ctx, *subformat_list,
					 sub_field_list,
					 sub_struct_size);
	    }
	    j++;
	}
	subformat_list++;
    }
    free(saved_subformat_list);
}

extern void
EVstone_install_conversion_action(cm, stone_id, target_format, incoming_format)
    CManager cm;
int stone_id;
IOFormat target_format;
IOFormat incoming_format;
{
    stone_type stone = &(cm->evp->stone_map[stone_id]);
    int a = stone->action_count++;
    int id_len;
    IOFormat format;

    char *server_id = get_server_ID_IOformat(incoming_format,
						     &id_len);
/*	    printf("Creating new DECODE action\n");*/
    stone->actions = realloc(stone->actions, 
			     sizeof(stone->actions[0]) * (a + 1));
    action *act = & stone->actions[a];
    act->requires_decoded = 0;
    act->action_type = Action_Decode;
    act->reference_format = incoming_format;
    act->queue_head = act->queue_tail = NULL;

    act->o.decode.context = create_IOsubcontext(cm->evp->root_context);
    format = get_format_app_IOcontext(act->o.decode.context, 
				      server_id, NULL);
    act->o.decode.decode_format = format;
    act->o.decode.target_reference_format = target_format;
    set_conversions(act->o.decode.context, format,
		    act->o.decode.target_reference_format);
}

static action *
determine_action(CManager cm, stone_type stone, event_item *event)
{
    int i;
    int nearest_proto_action = -1;
    CMtrace_out(cm, EVerbose, "Call to determine_action, event reference_format is %lx",
	   event->reference_format);
    for (i=0; i < stone->action_count; i++) {
	if (stone->actions[i].reference_format == event->reference_format) {
	    if (event->event_encoded && stone->actions[i].requires_decoded) {
		continue;
	    }
	    return &stone->actions[i];
	}
    }
/*    if ((stone->action_count != 0) || (stone->proto_action_count != 0)) {
	printf("no prebuilt action on stone %d\n", stone->local_id);
	}*/
    if (stone->proto_action_count > 0) {
	IOFormat * formatList;
	IOcompat_formats older_format = NULL;
	formatList =
	    (IOFormat *) malloc((stone->proto_action_count + 1) * sizeof(IOFormat));
	for (i = 0; i < stone->proto_action_count; i++) {
	    formatList[i] = stone->proto_actions[i].reference_format;
	}
	formatList[stone->proto_action_count] = NULL;
	nearest_proto_action = IOformat_compat_cmp(event->reference_format, 
						   formatList,
						   stone->proto_action_count,
						   &older_format);
	free(formatList);
    }

    if (nearest_proto_action == -1) {
	if (stone->default_action != -1) {
/*	    printf(" Returning ");
	    dump_action(stone, stone->default_action, "   ");*/
	    return &stone->actions[stone->default_action];
	}
	return NULL;
    }
    /* This format is to be bound with action nearest_proto_action */
    if (1) {
	if (event->event_encoded) {
	    /* create a decode action */
	    EVstone_install_conversion_action(cm, stone->local_id, 
					      stone->proto_actions[nearest_proto_action].reference_format, 
					      event->reference_format);
/*	    printf(" Returning ");
	    dump_action(stone, a, "   ");*/
	    return &stone->actions[stone->action_count-1];;
	}
    }
    return NULL;
}

static event_item *
decode_action(CManager cm, event_item *event, action *act)
{
    if (event->event_encoded) {
	if (decode_in_place_possible(act->o.decode.decode_format)) {
	    void *decode_buffer;
	    if (!decode_in_place_IOcontext(act->o.decode.context,
					   event->encoded_event, 
					   (void**) (long) &decode_buffer)) {
		printf("Decode failed\n");
		return 0;
	    }
	    event->decoded_event = decode_buffer;
	    event->event_encoded = 0;
	    return event;
	} else {
	    int decoded_length = this_IOrecord_length(act->o.decode.context, 
						      event->encoded_event, 
						      event->event_len);
	    CMbuffer cm_decode_buf = cm_get_data_buf(cm, decoded_length);
	    void *decode_buffer = cm_decode_buf->buffer;
	    decode_to_buffer_IOcontext(act->o.decode.context, 
				       event->encoded_event, decode_buffer);
/*	    cm_return_data_buf(conn->partial_buffer);*/
	    event->decoded_event = decode_buffer;
	    event->event_encoded = 0;
	    event->reference_format = act->o.decode.target_reference_format;
	    return event;
	}
    } else {
	assert(0);
    }
}

static void
dump_proto_action(stone_type stone, int a, const char *indent)
{
    proto_action *proto = &stone->proto_actions[a];
    printf("Proto-Action %d - %s\n", a, action_str[proto->action_type]);
}

static void
dump_action(stone_type stone, int a, const char *indent)
{
    action *act = &stone->actions[a];
    printf("Action %d - %s  ", a, action_str[act->action_type]);
    if (act->requires_decoded) {
	printf("requires decoded\n");
    } else {
	printf("accepts encoded\n");
    }
    printf("  reference format :");
    if (act->reference_format) {
	int id_len;
	print_server_ID(get_server_ID_IOformat(act->reference_format, &id_len));
    } else {
	printf(" NULL\n");
    }
    switch(act->action_type) {
    case Action_Output:
	printf("  Target: connection %lx, remote_stone_id %d, new %d, write_pending %d\n",
	       (long)(void*)act->o.out.conn, act->o.out.remote_stone_id, 
	       act->o.out.new, act->o.out.write_pending);
	break;
    case Action_Terminal:
	printf("  Terminal proto action number %d\n",
	       act->o.terminal_proto_action_number);
	break;
    case Action_Filter:
	printf("  Filter proto action number %d\n",
	       act->o.terminal_proto_action_number);
	break;
    case Action_Decode:
	printf("   Decoding action\n");
	break;
    case Action_Split:
	printf("    Split target stones: ");
	{
	    int i = 0;
	    while (act->o.split_stone_targets[i] != -1) {
		printf(" %d", act->o.split_stone_targets[i]); i++;
	    }
	    printf("\n");
	}
	break;
    }
}

static void
dump_stone(stone_type stone)
{
    int i;
    printf("Stone %lx, local ID %d, default action %d\n",
	   (long)stone, stone->local_id, stone->default_action);
    printf("  proto_action_count %d:\n", stone->proto_action_count);
    for (i=0; i< stone->proto_action_count; i++) {
	dump_proto_action(stone, i, "    ");
    }
    printf("  action_count %d:\n", stone->action_count);
    for (i=0; i< stone->action_count; i++) {
	dump_action(stone, i, "    ");
    }
}

static
int
internal_path_submit(CManager cm, int local_path_id, event_item *event)
{
    event_path_data evp = cm->evp;
    stone_type stone;
    action *act = NULL;

    assert(evpath_locked());
    if (evp->stone_count < local_path_id) {
	return -1;
    }
    stone = &(evp->stone_map[local_path_id]);
    act = determine_action(cm, stone, event);
    if (act == NULL) {
	printf("No action found for event %lx submitted to stone %d\n",
	       (long)event, local_path_id);
	dump_stone(stone);
	return 0;
    }
    if (act->action_type == Action_Decode) {
	CMtrace_out(cm, EVerbose, "Decoding event");
	event = decode_action(cm, event, act);
	act = determine_action(cm, stone, event);
    }
    CMtrace_out(cm, EVerbose, "Enqueueing event %lx on stone %d, action %lx",
		(long)event, local_path_id, (long)act);
    enqueue_event(cm, act, event);
    return 1;
}

static
int
process_local_actions(cm)
CManager cm;
{
    event_path_data evp = cm->evp;
    int s, a, more_pending = 0;
    CMtrace_out(cm, EVerbose, "Process local actions");
    for (s = 0; s < evp->stone_count; s++) {
	for (a=0 ; a < evp->stone_map[s].action_count; a++) {
	    action *act = &evp->stone_map[s].actions[a];
	    if (act->queue_head != NULL) {
		switch (act->action_type) {
		case Action_Terminal:
		case Action_Filter: {
		    /* the data should already be in the right format */
		    int proto = act->o.terminal_proto_action_number;
		    int out;
		    struct terminal_proto_vals *term = 
			&evp->stone_map[s].proto_actions[proto].t.term;
		    EVSimpleHandlerFunc handler = term->handler;
		    void *client_data = term->client_data;
		    event_item *event = dequeue_event(cm, act);
		    CMtrace_out(cm, EVerbose, "Executing terminal/filter event");
		    cm->evp->current_event_item = event;
		    out = (handler)(cm, event->decoded_event, client_data,
				    event->attrs);
		    cm->evp->current_event_item = NULL;
		    if (act->action_type == Action_Filter) {
			if (out) {
			    CMtrace_out(cm, EVerbose, "Filter passed event to stone %d, submitting", term->target_stone_id);
			    internal_path_submit(cm, 
						 term->target_stone_id,
						 event);
			} else {
			    CMtrace_out(cm, EVerbose, "Filter discarded event");
			}			    
			more_pending++;
		    } else {
			CMtrace_out(cm, EVerbose, "Finish terminal event");
		    }
		    return_event(evp, event);
		    break;
		}
		case Action_Split: {
		    int t = 0;
		    event_item *event = dequeue_event(cm, act);
		    while (act->o.split_stone_targets[t] != -1) {
			internal_path_submit(cm, 
					     act->o.split_stone_targets[t],
					     event);
			t++;
			more_pending++;
		    }
		    return_event(evp, event);
		  }
		case Action_Output:
		  /* handled elsewhere */
		  break;
		case Action_Decode:
		  assert(0);   /* handled elsewhere, shouldn't appear here */
		  break;
		}
	    }
	}
    }
    return more_pending;
}

static
int
process_output_actions(CManager cm)
{
    event_path_data evp = cm->evp;
    int s, a;
    CMtrace_out(cm, EVerbose, "Process output actions");
    for (s = 0; s < evp->stone_count; s++) {
	for (a=0 ; a < evp->stone_map[s].action_count; a++) {
	    action *act = &evp->stone_map[s].actions[a];
	    if ((act->action_type == Action_Output) && 
		(act->queue_head != NULL)) {
		event_item *event = dequeue_event(cm, act);
		CMtrace_out(cm, EVerbose, "Writing event to remote stone %d",
			    act->o.out.remote_stone_id);
		internal_write_event(act->o.out.conn, event->format,
				     &act->o.out.remote_stone_id, 4, 
				     event, event->attrs);
		return_event(evp, event);
	    }
	}
    }	    
    return 1;
}

extern EVaction
EVassoc_output_action(CManager cm, EVstone stone_num, attr_list contact_list,
		      EVstone remote_stone)
{
    event_path_data evp = cm->evp;
    stone_type stone = &evp->stone_map[stone_num];
    int action_num = stone->action_count++;
    stone->actions = realloc(stone->actions, 
				   (action_num + 1) * 
				   sizeof(stone->actions[0]));
    memset(&stone->actions[action_num], 0, 
	   sizeof(stone->actions[0]));
    stone->actions[action_num].action_type = Action_Output;
    stone->actions[action_num].o.out.conn = CMget_conn(cm, contact_list);
    stone->actions[action_num].o.out.remote_stone_id = remote_stone;
    stone->default_action = action_num;
    return action_num;
}

extern EVaction
EVassoc_split_action(CManager cm, EVstone stone_num, 
		     EVstone *target_stone_list)
{
    event_path_data evp = cm->evp;
    stone_type stone = &evp->stone_map[stone_num];
    int action_num = stone->action_count++;
    int target_count = 0, i;
    stone->actions = realloc(stone->actions, 
				   (action_num + 1) * 
				   sizeof(stone->actions[0]));
    memset(&stone->actions[action_num], 0, 
	   sizeof(stone->actions[0]));
    stone->actions[action_num].action_type = Action_Split;
    while (target_stone_list && (target_stone_list[target_count] != -1)) {
	target_count++;
    }
    stone->actions[action_num].o.split_stone_targets = 
	malloc((target_count + 1) * sizeof(EVstone));
    for (i=0; i < target_count; i++) {
	stone->actions[action_num].o.split_stone_targets[i] = 
	    target_stone_list[i];
    }
    stone->actions[action_num].o.split_stone_targets[i] = -1;
    stone->default_action = action_num;
    return action_num;
}

extern int
EVaction_add_split_target(CManager cm, EVstone stone_num, 
			  EVaction action_num, EVstone new_stone_target)
{
    event_path_data evp = cm->evp;
    stone_type stone = &evp->stone_map[stone_num];
    EVstone *target_stone_list;
    int target_count = 0;
    if (stone->actions[action_num].action_type != Action_Split ) {
	printf("Not split action\n");
	return 0;
    }
    target_stone_list = stone->actions[action_num].o.split_stone_targets;
    while (target_stone_list && (target_stone_list[target_count] != -1)) {
	target_count++;
    }
    target_stone_list = realloc(target_stone_list, 
				(target_count + 2) * sizeof(EVstone));
    target_stone_list[target_count] = new_stone_target;
    target_stone_list[target_count+1] = -1;
    stone->actions[action_num].o.split_stone_targets = target_stone_list;
    return 1;
}

extern void
EVaction_remove_split_target(CManager cm, EVstone stone_num, 
			  EVaction action_num, EVstone stone_target)
{
    event_path_data evp = cm->evp;
    stone_type stone = &evp->stone_map[stone_num];
    EVstone *target_stone_list;
    int target_count = 0;
    if (stone->actions[action_num].action_type != Action_Split ) {
	printf("Not split action\n");
    }
    target_stone_list = stone->actions[action_num].o.split_stone_targets;
    if (!target_stone_list) return;
    while (target_stone_list[target_count] != stone_target) {
	target_count++;
    }
    while (target_stone_list[target_count+1] != -1 ) {
	/* move them down, overwriting target */
	target_stone_list[target_count] = target_stone_list[target_count+1];
	target_count++;
    }
    target_stone_list[target_count] = -1;
}

static int
register_subformats(context, field_list, sub_list)
IOContext context;
IOFieldList field_list;
CMFormatList sub_list;
{
    char **subformats = get_subformat_names(field_list);
    char **save_subformats = subformats;

    if (subformats != NULL) {
	while (*subformats != NULL) {
	    int i = 0;
	    if (get_IOformat_by_name_IOcontext(context, *subformats) != NULL) {
		/* already registered this subformat */
		goto next_format;
	    }
	    while (sub_list && (sub_list[i].format_name != NULL)) {
		if (strcmp(sub_list[i].format_name, *subformats) == 0) {
		    IOFormat tmp;
		    if (register_subformats(context, sub_list[i].field_list,
					      sub_list) != 1) {
			fprintf(stderr, "Format registration failed for subformat \"%s\"\n",
				sub_list[i].format_name);
			return 0;
		    }
		    tmp = register_IOcontext_format(*subformats,
						    sub_list[i].field_list,
						    context);
		    if (tmp == NULL) {
			fprintf(stderr, "Format registration failed for subformat \"%s\"\n",
				sub_list[i].format_name);
			return 0;
		    }
		    goto next_format;
		}
		i++;
	    }
	    fprintf(stderr, "Subformat \"%s\" not found in format list\n",
		    *subformats);
	    return 0;
	next_format:
	    free(*subformats);
	    subformats++;
	}
    }
    free(save_subformats);
    return 1;
}

static IOFormat
register_format_set(CManager cm, CMFormatList list, IOContext *context_ptr)
{
    event_path_data evp = cm->evp;
    char *server_id;
    int id_len;
    IOFormat format;
    IOContext tmp_context = create_IOsubcontext(evp->root_context);

    if (register_subformats(tmp_context, list[0].field_list, list) != 1) {
	fprintf(stderr, "Format registration failed for format \"%s\"\n",
		list[0].format_name);
	return 0;
    }
    format = register_opt_format(list[0].format_name, 
				 list[0].field_list, NULL, 
				 tmp_context);
    server_id = get_server_ID_IOformat(format, &id_len);
    if (context_ptr == NULL) {
	/* replace original ref with ref in base context */
	format = get_format_app_IOcontext(evp->root_context, server_id, NULL);

	free_IOsubcontext(tmp_context);
    } else {
	*context_ptr = tmp_context;
    }
    return format;
}

static void
EVauto_submit_func(CManager cm, void* vstone)
{
    int stone_num = (long) vstone;
    event_item *event = get_free_event(cm->evp);
    event->event_encoded = 0;
    event->decoded_event = NULL;
    event->reference_format = NULL;
    event->format = NULL;
    event->free_func = NULL;
    event->attrs = NULL;
    internal_path_submit(cm, stone_num, event);
    return_event(cm->evp, event);
    while (process_local_actions(cm));
    process_output_actions(cm);
}

extern void
EVenable_auto_stone(CManager cm, EVstone stone_num, int period_sec, 
		    int period_usec)
{
    CMTaskHandle handle = CMadd_periodic_task(cm, period_sec, period_usec,
					      EVauto_submit_func, 
					      (void*)(long)stone_num);
    stone_type stone = &cm->evp->stone_map[stone_num];
    stone->periodic_handle = handle;
}



extern EVsource
EVcreate_submit_handle(CManager cm, EVstone stone, CMFormatList data_format)
{
    EVsource source = malloc(sizeof(*source));
    memset(source, 0, sizeof(*source));
    source->local_stone_id = stone;
    source->cm = cm;
    if (data_format != NULL) {
	source->format = CMregister_format(cm, data_format[0].format_name,
					   data_format[0].field_list,
					   data_format);
	source->reference_format = register_format_set(cm, data_format, NULL);
    };
    return source;
}

extern EVsource
EVcreate_submit_handle_free(CManager cm, EVstone stone, 
			    CMFormatList data_format, 
			    EVFreeFunction free_func, void *free_data)
{
    EVsource source = malloc(sizeof(*source));
    memset(source, 0, sizeof(*source));
    source->local_stone_id = stone;
    source->cm = cm;
    source->format = CMregister_format(cm, data_format[0].format_name,
					    data_format[0].field_list, data_format);
    source->reference_format = register_format_set(cm, data_format, NULL);
    source->free_func = free_func;
    source->free_data = free_data;
    return source;
}

static event_item *
get_free_event(event_path_data evp)
{
    event_item *event = malloc(sizeof(*event));
    memset(event, 0, sizeof(event_item));
    event->ref_count = 1;
    return event;
}

static void
return_event(event_path_data evp, event_item *event)
{
    event->ref_count--;
    if (event->ref_count == 0) {
	/* return event memory */
	switch (event->contents) {
	case Event_CM_Owned:
	    CMreturn_buffer(event->cm, event->decoded_event);
	    break;
	case Event_Freeable:
	    (event->free_func)(event->decoded_event, event->free_arg);
	    break;
	case Event_App_Owned:
	    if (event->free_func) {
		(event->free_func)(event->free_arg, NULL);
	    }
	    break;
	}
	free(event);
    }
}

static void
reference_event(event_item *event)
{
    event->ref_count++;
}

extern void
internal_cm_network_submit(CManager cm, CMbuffer cm_data_buf, 
			   attr_list attrs, CMConnection conn, 
			   void *buffer, int stone_id)
{
    event_path_data evp = cm->evp;
    event_item *event = get_free_event(evp);
    event->contents = Event_CM_Owned;
    event->event_encoded = 1;
    event->encoded_event = buffer;
    event->reference_format = get_format_app_IOcontext(evp->root_context, 
					     buffer, conn);
    event->attrs = attrs;
    event->format = NULL;
    CMtrace_out(cm, EVerbose, "Event coming in from network to stone %d", 
		stone_id);
    CMtake_buffer(cm, buffer);
    event->cm = cm;
    internal_path_submit(cm, stone_id, event);
    return_event(evp, event);
    while (process_local_actions(cm));
    process_output_actions(cm);
}

extern void
EVsubmit_general(EVsource source, void *data, EVFreeFunction free_func, 
		 attr_list attrs)
{
    event_item *event = get_free_event(source->cm->evp);
    event->contents = Event_App_Owned;
    event->event_encoded = 0;
    event->decoded_event = data;
    event->reference_format = source->reference_format;
    event->format = source->format;
    event->free_func = free_func;
    event->free_arg = data;
    internal_path_submit(source->cm, source->local_stone_id, event);
    return_event(source->cm->evp, event);
    while (process_local_actions(source->cm));
    process_output_actions(source->cm);
}
    
void
EVsubmit(EVsource source, void *data, attr_list attrs)
{
    event_item *event = get_free_event(source->cm->evp);
    if (source->free_func != NULL) {
	event->contents = Event_Freeable;
    } else {
	event->contents = Event_App_Owned;
    }
    event->event_encoded = 0;
    event->decoded_event = data;
    event->reference_format = source->reference_format;
    event->format = source->format;
    event->free_func = source->free_func;
    event->free_arg = source->free_data;
    event->attrs = attrs;
    internal_path_submit(source->cm, source->local_stone_id, event);
    return_event(source->cm->evp, event);
    while (process_local_actions(source->cm));
    process_output_actions(source->cm);
}

static void
free_evp(CManager cm, void *not_used)
{
    event_path_data evp = cm->evp;
    int s;
    cm->evp = NULL;
    if (evp == NULL) return;
    for (s = 0 ; s < evp->stone_count; s++) {
	int a;
	for (a = 0 ; a < evp->stone_map[s].action_count; a++) {
	    action *act = &evp->stone_map[s].actions[a];
	    switch(act->action_type) {
	    case Action_Output:
		if (act->o.out.remote_path) 
		    free(act->o.out.remote_path);
		break;
	    case Action_Terminal:
		break;
	    case Action_Filter:
		break;
	    case Action_Decode:
		free_IOcontext(act->o.decode.context);
		break;
	    case Action_Split:
		free(act->o.split_stone_targets);
		break;
	    }
	}
    }
    free(evp->stone_map);
    free(evp->output_actions);
    free_IOcontext(evp->root_context);
    while (evp->queue_items_free_list != NULL) {
	queue_item *tmp = evp->queue_items_free_list->next;
	free(evp->queue_items_free_list->next);
	evp->queue_items_free_list = tmp;
    }
    thr_mutex_free(evp->lock);
}

void
EVPinit(CManager cm)
{
    cm->evp = CMmalloc(sizeof( struct _event_path_data));
    memset(cm->evp, 0, sizeof( struct _event_path_data));
    cm->evp->root_context = create_IOcontext();
    cm->evp->queue_items_free_list = NULL;
    cm->evp->lock = thr_mutex_alloc();
    internal_add_shutdown_task(cm, free_evp, NULL);
}

    
extern int
EVtake_event_buffer(CManager cm, void *event)
{
    queue_item *item;
    event_item *cur = cm->evp->current_event_item;
    event_path_data evp = cm->evp;

    if (cur == NULL) {
	fprintf(stderr,
		"No event handler with takeable buffer executing on this CM.\n");
	return 0;
    }
    if (!((cur->decoded_event <= event) &&
	  ((char *) event <= ((char *) cur->decoded_event + cur->event_len)))){
	fprintf(stderr,
		"Event address (%lx) in EVtake_event_buffer does not match currently executing event on this CM.\n",
		(long) event);
	return 0;
    }
/*    if (cur->block_rec == NULL) {
	static int take_event_warning = 0;
	if (take_event_warning == 0) {
	    fprintf(stderr,
		    "Warning:  EVtake_event_buffer called on an event submitted with \n    EVsubmit_event(), EVsubmit_typed_event() or EVsubmit_eventV() .\n    This violates ECho event data memory handling requirements.  See \n    http://www.cc.gatech.edu/systems/projects/ECho/event_memory.html\n");
	    take_event_warning++;
	}
	return 0;
    }
*/
    if (evp->queue_items_free_list == NULL) {
	item = malloc(sizeof(*item));
    } else {
	item = evp->queue_items_free_list;
	evp->queue_items_free_list = item->next;
    }
    item->item = cur;
    reference_event(cm->evp->current_event_item);
    item->next = evp->taken_events_list;
    evp->taken_events_list = item;
    return 1;
}

void
EVreturn_event_buffer(cm, event)
CManager cm;
void *event;
{
    event_path_data evp = cm->evp;
    queue_item *tmp, *last = NULL;
    /* search through list for event and then dereference it */

    tmp = evp->taken_events_list;
    while (tmp != NULL) {
	if ((tmp->item->decoded_event <= event) &&
	    ((char *) event <= ((char *) tmp->item->decoded_event + tmp->item->event_len))) {
	    if (last == NULL) {
		evp->taken_events_list = tmp->next;
	    } else {
		last->next = tmp->next;
	    }
	    return_event(cm->evp, event);
	    tmp->next = evp->queue_items_free_list;
	    evp->queue_items_free_list = tmp;
	    return;
	}
	last = tmp;
	tmp = tmp->next;
    }
    fprintf(stderr, "Event %lx not found in taken events list\n",
	    (long) event);
}

extern IOFormat
EVget_src_ref_format(EVsource source)
{
    return source->reference_format;
}
