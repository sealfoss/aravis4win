/* SPDX-License-Identifier:Unlicense */

#include <arv.h>

static void
child_list_test (void)
{
	ArvDevice *device;
	ArvGc *genicam;
	GError *error = NULL;
        ArvGcNode *node;
        ArvDomNodeList *children;
        ArvDomNode *item;
        unsigned int length, i;

	device = arv_fake_device_new ("TEST0", &error);
	g_assert (ARV_IS_FAKE_DEVICE (device));
	g_assert (error == NULL);

	genicam = arv_device_get_genicam (device);
	g_assert (ARV_IS_GC (genicam));

        node = arv_gc_get_node (genicam, "Root");

        children = arv_dom_node_get_child_nodes(ARV_DOM_NODE(node));
        length = arv_dom_node_list_get_length(children);
        g_assert (length > 0);

        for (i = 0; i < length; ++i) {
                item = arv_dom_node_list_get_item(children, i);
                g_assert (ARV_IS_DOM_NODE (item));
        }

        g_object_unref (device);
}

/* GenICam descriptions parsed by several threads at once, as when cameras are opened in parallel threads: each parse
 * has a state of its own, and the table of document types is created once, by whichever thread needs it first. */

#define N_PARSE_THREADS		4
#define N_PARSES_PER_THREAD	100

typedef struct {
	const char *xml;
	size_t size;
	guint n_nodes;
	gint n_failures;
} ParseData;

static guint
count_nodes (ArvDomNode *node)
{
	ArvDomNode *child;
	guint n_nodes = 1;

	for (child = arv_dom_node_get_first_child (node); child != NULL; child = arv_dom_node_get_next_sibling (child))
		n_nodes += count_nodes (child);

	return n_nodes;
}

static gpointer
parse_thread (gpointer user_data)
{
	ParseData *data = user_data;
	guint i;

	for (i = 0; i < N_PARSES_PER_THREAD; i++) {
		ArvGc *genicam;

		/* The whole tree, in this document only */
		genicam = arv_gc_new (NULL, data->xml, data->size);
		if (!ARV_IS_GC (genicam) || count_nodes (ARV_DOM_NODE (genicam)) != data->n_nodes)
			g_atomic_int_inc (&data->n_failures);
		g_clear_object (&genicam);
	}

	return NULL;
}

static void
parallel_parse_test (void)
{
	GThread *threads[N_PARSE_THREADS];
	ParseData data = {0};
	ArvGc *genicam;
	char *xml = NULL;
	gsize size = 0;
	guint i;

	g_assert_true (g_file_get_contents (GENICAM_FILENAME, &xml, &size, NULL));
	data.xml = xml;
	data.size = size;

	genicam = arv_gc_new (NULL, xml, size);
	g_assert_true (ARV_IS_GC (genicam));
	data.n_nodes = count_nodes (ARV_DOM_NODE (genicam));
	g_assert_cmpuint (data.n_nodes, >, 100);
	g_object_unref (genicam);

	/* The first parses of the threads create the document types again */
	arv_dom_implementation_cleanup ();

	for (i = 0; i < N_PARSE_THREADS; i++)
		threads[i] = g_thread_new ("parse", parse_thread, &data);
	for (i = 0; i < N_PARSE_THREADS; i++)
		g_thread_join (threads[i]);

	g_assert_cmpint (data.n_failures, ==, 0);

	g_free (xml);
}

int
main (int argc, char *argv[])
{
	int result;

	g_test_init (&argc, &argv, NULL);

	arv_set_fake_camera_genicam_filename (GENICAM_FILENAME);

	g_test_add_func ("/dom/child-list", child_list_test);
	g_test_add_func ("/dom/parallel-parse", parallel_parse_test);

	result = g_test_run();

	arv_shutdown ();

	return result;
}
