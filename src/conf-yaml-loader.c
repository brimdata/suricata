/* Copyright (c) 2009 Open Information Security Foundation */

/**
 * \file
 *
 * \author Endace Technology Limited - Jason Ish <jason.ish@endace.com>
 *
 * YAML configuration loader.
 */

#include <yaml.h>
#include "suricata-common.h"
#include "conf.h"
#include "util-debug.h"
#include "util-unittest.h"

#define YAML_VERSION_MAJOR 1
#define YAML_VERSION_MINOR 1

/* Sometimes we'll have to create a node name on the fly (integer
 * conversion, etc), so this is a default length to allocate that will
 * work most of the time. */
#define DEFAULT_NAME_LEN 16

/* Configuration processing states. */
enum conf_state {
    CONF_KEY = 0,
    CONF_VAL,
};

/**
 * \brief Parse a YAML layer.
 *
 * \param parser A pointer to an active yaml_parser_t.
 * \param parent The parent configuration node.
 *
 * \retval 0 on success, -1 on failure.
 */
static int
ConfYamlParse(yaml_parser_t *parser, ConfNode *parent, int inseq)
{
    ConfNode *node = parent;
    yaml_event_t event;
    int done = 0;
    int state = 0;
    int seq_idx = 0;

    while (!done) {
        if (!yaml_parser_parse(parser, &event)) {
            fprintf(stderr, "Failed to parse configuration file: %s\n",
                parser->problem);
            return -1;
        }

        if (event.type == YAML_DOCUMENT_START_EVENT) {
            /* Verify YAML version - its more likely to be a valid
             * Suricata configuration file if the version is
             * correct. */
            yaml_version_directive_t *ver =
                event.data.document_start.version_directive;
            if (ver == NULL) {
                fprintf(stderr, "ERROR: Invalid configuration file.\n\n");
                fprintf(stderr, "The configuration file must begin with the following two lines:\n\n");
                fprintf(stderr, "%%YAML 1.1\n---\n\n");
                goto fail;
            }
            int major = event.data.document_start.version_directive->major;
            int minor = event.data.document_start.version_directive->minor;
            if (!(major == YAML_VERSION_MAJOR && minor == YAML_VERSION_MINOR)) {
                fprintf(stderr, "ERROR: Invalid YAML version.  Must be 1.1\n");
                goto fail;
            }
        }
        else if (event.type == YAML_SCALAR_EVENT) {
            char *value = (char *)event.data.scalar.value;
            SCLogDebug("event.type = YAML_SCALAR_EVENT (%s) inseq=%d\n",
                value, inseq);
            if (inseq) {
                ConfNode *seq_node = ConfNodeNew();
                seq_node->name = calloc(1, DEFAULT_NAME_LEN);
                snprintf(seq_node->name, DEFAULT_NAME_LEN, "%d", seq_idx++);
                seq_node->val = strdup(value);
                TAILQ_INSERT_TAIL(&parent->head, seq_node, next);
            }
            else {
                if (state == CONF_KEY) {
                    /* If the node already exists, check if we can
                     * override it.  If we can, free it then continue
                     * otherwise move onto the next configuration
                     * parameter. */
                    ConfNode *n0 = ConfNodeLookupChild(parent, value);
                    if (n0 != NULL) {
                        if (n0->allow_override) {
                            ConfNodeRemove(n0);
                        }
                        else {
                            state = CONF_VAL;
                            goto next;
                        }
                    }
                    if (parent->is_seq) {
                        if (parent->val == NULL) {
                            parent->val = strdup(value);
                        }
                    }
                    node = ConfNodeNew();
                    node->name = strdup(value);
                    TAILQ_INSERT_TAIL(&parent->head, node, next);
                    state = CONF_VAL;
                }
                else {
                    node->val = strdup(value);
                    state = CONF_KEY;
                }
            }
        }
        else if (event.type == YAML_SEQUENCE_START_EVENT) {
            SCLogDebug("event.type = YAML_SEQUENCE_START_EVENT\n");
            if (ConfYamlParse(parser, node, 1) != 0)
                goto fail;
            state = CONF_KEY;
        }
        else if (event.type == YAML_SEQUENCE_END_EVENT) {
            SCLogDebug("event.type = YAML_SEQUENCE_END_EVENT\n");
            return 0;
        }
        else if (event.type == YAML_MAPPING_START_EVENT) {
            SCLogDebug("event.type = YAML_MAPPING_START_EVENT\n");
            if (inseq) {
                ConfNode *seq_node = ConfNodeNew();
                seq_node->is_seq = 1;
                seq_node->name = calloc(1, DEFAULT_NAME_LEN);
                snprintf(seq_node->name, DEFAULT_NAME_LEN, "%d", seq_idx++);
                TAILQ_INSERT_TAIL(&node->head, seq_node, next);
                ConfYamlParse(parser, seq_node, 0);
            }
            else {
                ConfYamlParse(parser, node, inseq);
            }
            state = CONF_KEY;
        }
        else if (event.type == YAML_MAPPING_END_EVENT) {
            SCLogDebug("event.type = YAML_MAPPING_END_EVENT\n");
            done = 1;
        }
        else if (event.type == YAML_STREAM_END_EVENT) {
            done = 1;
        }

    next:
        yaml_event_delete(&event);
        continue;

    fail:
        yaml_event_delete(&event);
        return -1;
    }

    return 0;
}

/**
 * \brief Load configuration from a YAML file.
 *
 * This function will load a configuration file.  On failure -1 will
 * be returned and it is suggested that the program then exit.  Any
 * errors while loading the configuration file will have already been
 * logged.
 *
 * \param filename Filename of configuration file to load.
 *
 * \retval 0 on success, -1 on failure.
 */
int
ConfYamlLoadFile(const char *filename)
{
    FILE *infile;
    yaml_parser_t parser;
    int ret;
    ConfNode *root = ConfGetRootNode();

    if (yaml_parser_initialize(&parser) != 1) {
        fprintf(stderr, "Failed to initialize yaml parser.\n");
        return -1;
    }

    infile = fopen(filename, "r");
    if (infile == NULL) {
        fprintf(stderr, "Failed to open file: %s: %s\n", filename,
            strerror(errno));
        yaml_parser_delete(&parser);
        return -1;
    }
    yaml_parser_set_input_file(&parser, infile);
    ret = ConfYamlParse(&parser, root, 0);
    yaml_parser_delete(&parser);
    fclose(infile);

    return ret;
}

/**
 * \brief Load configuration from a YAML string.
 */
int
ConfYamlLoadString(const char *string, size_t len)
{
    ConfNode *root = ConfGetRootNode();
    yaml_parser_t parser;
    int ret;

    if (yaml_parser_initialize(&parser) != 1) {
        fprintf(stderr, "Failed to initialize yaml parser.\n");
        exit(EXIT_FAILURE);
    }
    yaml_parser_set_input_string(&parser, (const unsigned char *)string, len);
    ret = ConfYamlParse(&parser, root, 0);
    yaml_parser_delete(&parser);

    return ret;
}

#ifdef UNITTESTS

static int
ConfYamlRuleFileTest(void)
{
    char input[] = "\
%YAML 1.1\n\
---\n\
rule-files:\n\
  - netbios.rules\n\
  - x11.rules\n\
\n\
default-log-dir: /tmp\n\
";

    ConfCreateContextBackup();
    ConfInit();

    ConfYamlLoadString(input, strlen(input));

    ConfNode *node;
    node = ConfGetNode("rule-files");
    if (node == NULL)
        return 0;
    if (TAILQ_EMPTY(&node->head))
        return 0;
    int i = 0;
    ConfNode *filename;
    TAILQ_FOREACH(filename, &node->head, next) {
        if (i == 0) {
            if (strcmp(filename->val, "netbios.rules") != 0)
                return 0;
        }
        else if (i == 1) {
            if (strcmp(filename->val, "x11.rules") != 0)
                return 0;
        }
        else {
            return 0;
        }
        i++;
    }

    ConfDeInit();
    ConfRestoreContextBackup();

    return 1;
}

static int
ConfYamlLoggingOutputTest(void)
{
    char input[] = "\
%YAML 1.1\n\
---\n\
logging:\n\
  output:\n\
    - interface: console\n\
      log-level: error\n\
    - interface: syslog\n\
      facility: local4\n\
      log-level: info\n\
";

    ConfCreateContextBackup();
    ConfInit();

    ConfYamlLoadString(input, strlen(input));

    ConfNode *outputs;
    outputs = ConfGetNode("logging.output");
    if (outputs == NULL)
        return 0;

    ConfNode *output;
    ConfNode *output_param;

    output = TAILQ_FIRST(&outputs->head);
    if (output == NULL)
        return 0;
    if (strcmp(output->name, "0") != 0)
        return 0;
    output_param = TAILQ_FIRST(&output->head);
    if (output_param == NULL)
        return 0;
    if (strcmp(output_param->name, "interface") != 0)
        return 0;
    if (strcmp(output_param->val, "console") != 0)
        return 0;
    output_param = TAILQ_NEXT(output_param, next);
    if (strcmp(output_param->name, "log-level") != 0)
        return 0;
    if (strcmp(output_param->val, "error") != 0)
        return 0;

    output = TAILQ_NEXT(output, next);
    if (output == NULL)
        return 0;
    if (strcmp(output->name, "1") != 0)
        return 0;
    output_param = TAILQ_FIRST(&output->head);
    if (output_param == NULL)
        return 0;
    if (strcmp(output_param->name, "interface") != 0)
        return 0;
    if (strcmp(output_param->val, "syslog") != 0)
        return 0;
    output_param = TAILQ_NEXT(output_param, next);
    if (strcmp(output_param->name, "facility") != 0)
        return 0;
    if (strcmp(output_param->val, "local4") != 0)
        return 0;
    output_param = TAILQ_NEXT(output_param, next);
    if (strcmp(output_param->name, "log-level") != 0)
        return 0;
    if (strcmp(output_param->val, "info") != 0)
        return 0;

    ConfDeInit();
    ConfRestoreContextBackup();

    return 1;
}

/**
 * Try to load something that is not a valid YAML file.
 */
static int
ConfYamlNonYamlFileTest(void)
{
    ConfCreateContextBackup();
    ConfInit();

    if (ConfYamlLoadFile("/etc/passwd") != -1)
        return 0;

    ConfDeInit();
    ConfRestoreContextBackup();

    return 1;
}

static int
ConfYamlBadYamlVersionTest(void)
{
    char input[] = "\
%YAML 9.9\n\
---\n\
logging:\n\
  output:\n\
    - interface: console\n\
      log-level: error\n\
    - interface: syslog\n\
      facility: local4\n\
      log-level: info\n\
";

    ConfCreateContextBackup();
    ConfInit();

    if (ConfYamlLoadString(input, strlen(input)) != -1)
        return 0;

    ConfDeInit();
    ConfRestoreContextBackup();

    return 1;
}

static int
ConfYamlSecondLevelSequenceTest(void)
{
    char input[] = "\
%YAML 1.1\n\
---\n\
libhtp:\n\
  server-config:\n\
    - apache-php:\n\
        address: [\"192.168.1.0/24\"]\n\
        personality: [\"Apache_2_2\", \"PHP_5_3\"]\n\
        path-parsing: [\"compress_separators\", \"lowercase\"]\n\
    - iis-php:\n\
        address:\n\
          - 192.168.0.0/24\n\
\n\
        personality:\n\
          - IIS_7_0\n\
          - PHP_5_3\n\
\n\
        path-parsing:\n\
          - compress_separators\n\
";

    ConfCreateContextBackup();
    ConfInit();

    if (ConfYamlLoadString(input, strlen(input)) != 0)
        return 0;

    ConfNode *outputs;
    outputs = ConfGetNode("libhtp.server-config");
    if (outputs == NULL)
        return 0;

    ConfNode *node;

    node = TAILQ_FIRST(&outputs->head);
    if (node == NULL)
        return 0;
    if (strcmp(node->name, "0") != 0)
        return 0;
    node = TAILQ_FIRST(&node->head);
    if (node == NULL)
        return 0;
    if (strcmp(node->name, "apache-php") != 0)
        return 0;

    node = ConfNodeLookupChild(node, "address");
    if (node == NULL)
        return 0;
    node = TAILQ_FIRST(&node->head);
    if (node == NULL)
        return 0;
    if (strcmp(node->name, "0") != 0)
        return 0;
    if (strcmp(node->val, "192.168.1.0/24") != 0)
        return 0;

    ConfDeInit();
    ConfRestoreContextBackup();

    return 1;
}

#endif /* UNITTESTS */

void
ConfYamlRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("ConfYamlRuleFileTest", ConfYamlRuleFileTest, 1);
    UtRegisterTest("ConfYamlLoggingOutputTest", ConfYamlLoggingOutputTest, 1);
    UtRegisterTest("ConfYamlNonYamlFileTest", ConfYamlNonYamlFileTest, 1);
    UtRegisterTest("ConfYamlBadYamlVersionTest", ConfYamlBadYamlVersionTest, 1);
    UtRegisterTest("ConfYamlSecondLevelSequenceTest",
        ConfYamlSecondLevelSequenceTest, 1);
#endif /* UNITTESTS */
}
