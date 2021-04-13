#include <stdlib.h>
#include <string.h>

#include "embedded_cli.h"

#define PREPARE_IMPL(t) \
  EmbeddedCliImpl* impl = (EmbeddedCliImpl*)t->_impl;

#define IS_FLAG_SET(flags, flag) (((flags) & (flag)) != 0)

#define SET_FLAG(flags, flag) ((flags) |= (flag))

#define UNSET_FLAG(flags, flag) ((flags) &= ~(flag))

/**
 * Marks binding as candidate for autocompletion
 * This flag is updated each time getAutocompletedCommand is called
 */
#define BINDING_FLAG_AUTOCOMPLETE 1u

/**
 * Indicates that rx buffer overflow happened. In such case last command
 * that wasn't finished (no \r or \n were received) will be discarded
 */
#define CLI_FLAG_OVERFLOW 0x01u

/**
 * Indicates that initialization is completed. Initialization is completed in
 * first call to process and needed, for example, to print invitation message.
 */
#define CLI_FLAG_INIT_COMPLETE 0x02u

/**
 * Indicates that CLI structure and internal structures were allocated with
 * malloc and should bre freed
 */
#define CLI_FLAG_ALLOCATED 0x04u

/**
 * Indicates that CLI structure and internal structures were allocated with
 * malloc and should bre freed
 */
#define CLI_FLAG_ESCAPE_MODE 0x08u

typedef struct EmbeddedCliImpl EmbeddedCliImpl;
typedef struct AutocompletedCommand AutocompletedCommand;
typedef struct FifoBuf FifoBuf;
typedef struct CliHistory CliHistory;

struct FifoBuf {
    char *buf;
    /**
     * Position of first element in buffer. From this position elements are taken
     */
    uint16_t front;
    /**
     * Position after last element. At this position new elements are inserted
     */
    uint16_t back;
    /**
     * Size of buffer
     */
    uint16_t size;
};

struct CliHistory {
    /**
     * Items in buffer are separated by null-chars
     */
    char *buf;

    /**
     * Total size of buffer
     */
    uint16_t bufferSize;

    /**
     * Number of items in buffer
     * Items are counted from top to bottom (and are 1 based).
     * So the most recent item is 1 and the oldest is itemCount.
     */
    uint16_t itemsCount;
};

struct EmbeddedCliImpl {
    /**
     * Invitation string. Is printed at the beginning of each line with user
     * input
     */
    const char *invitation;

    CliHistory history;

    /**
     * Buffer for storing received chars.
     * Chars are stored in FIFO mode.
     */
    FifoBuf rxBuffer;

    /**
     * Buffer for current command
     */
    char *cmdBuffer;

    /**
     * Size of current command
     */
    uint16_t cmdSize;

    /**
     * Total size of command buffer
     */
    uint16_t cmdMaxSize;

    CliCommandBinding *bindings;

    /**
     * Flags for each binding. Sizes are the same as for bindings array
     */
    uint8_t *bindingsFlags;

    uint16_t bindingsCount;

    uint16_t maxBindingsCount;

    /**
     * Stores last character that was processed.
     */
    char lastChar;

    /**
     * Total length of input line. This doesn't include invitation but
     * includes current command and its live autocompletion
     */
    uint16_t inputLineLength;

    /**
     * Flags are defined as CLI_FLAG_*
     */
    uint8_t flags;
};

struct AutocompletedCommand {
    /**
     * Name of autocompleted command (or first candidate for autocompletion if
     * there are multiple candidates).
     * NULL if autocomplete not possible.
     */
    const char *firstCandidate;

    /**
     * Number of characters that can be completed safely. For example, if there
     * are two possible commands "get-led" and "get-adc", then for prefix "g"
     * autocompletedLen will be 4. If there are only one candidate, this number
     * is always equal to length of the command.
     */
    size_t autocompletedLen;

    /**
     * Total number of candidates for autocompletion
     */
    uint16_t candidateCount;
};

static EmbeddedCliConfig defaultConfig;

/**
 * Number of commands that cli adds. Commands:
 * - help
 */
static const uint16_t cliInternalBindingCount = 1;

static const char *lineBreak = "\r\n";

/**
 * Process escaped character. After receiving ESC+[ sequence, all chars up to
 * ending character are sent to this function
 * @param cli
 * @param c
 */
static void onEscapedInput(EmbeddedCli *cli, char c);

/**
 * Process input character. Character is valid displayable char and should be
 * added to current command string and displayed to client.
 * @param cli
 * @param c
 */
static void onCharInput(EmbeddedCli *cli, char c);

/**
 * Process control character (like \r or \n) possibly altering state of current
 * command or executing onCommand callback.
 * @param cli
 * @param c
 */
static void onControlInput(EmbeddedCli *cli, char c);

/**
 * Parse command in buffer and execute callback
 * @param cli
 */
static void parseCommand(EmbeddedCli *cli);

/**
 * Setup bindings for internal commands, like help
 * @param cli
 */
static void initInternalBindings(EmbeddedCli *cli);

/**
 * Show help for given tokens (or default help if no tokens)
 * @param cli
 * @param tokens
 * @param context - not used
 */
static void onHelp(EmbeddedCli *cli, char *tokens, void *context);

/**
 * Show error about unknown command
 * @param cli
 * @param name
 */
static void onUnknownCommand(EmbeddedCli *cli, const char *name);

/**
 * Return autocompleted command for given prefix.
 * Prefix is compared to all known command bindings and autocompleted result
 * is returned
 * @param cli
 * @param prefix
 * @return
 */
static AutocompletedCommand getAutocompletedCommand(EmbeddedCli *cli, const char *prefix);

/**
 * Prints autocompletion result while keeping current command unchanged
 * Prints only if autocompletion is present and only one candidate exists.
 * @param cli
 */
static void printLiveAutocompletion(EmbeddedCli *cli);

/**
 * Handles autocomplete request. If autocomplete possible - fills current
 * command with autocompleted command. When multiple commands satisfy entered
 * prefix, they are printed to output.
 * @param cli
 */
static void onAutocompleteRequest(EmbeddedCli *cli);

/**
 * Removes all input from current line (replaces it with whitespaces)
 * And places cursor at the beginning of the line
 * @param cli
 */
static void clearCurrentLine(EmbeddedCli *cli);

/**
 * Write given string to cli output
 * @param cli
 * @param str
 */
static void writeToOutput(EmbeddedCli *cli, const char *str);

/**
 * Returns true if provided char is a supported control char:
 * \r, \n, \b or 0x7F (treated as \b)
 * @param c
 * @return
 */
static bool isControlChar(char c);

/**
 * Returns true if provided char is a valid displayable character:
 * a-z, A-Z, 0-9, whitespace, punctuation, etc.
 * Currently only ASCII is supported
 * @param c
 * @return
 */
static bool isDisplayableChar(char c);

/**
 * How many elements are currently available in buffer
 * @param buffer
 * @return number of elements
 */
static uint16_t fifoBufAvailable(FifoBuf *buffer);

/**
 * Return first character from buffer and remove it from buffer
 * Buffer must be non-empty, otherwise 0 is returned
 * @param buffer
 * @return
 */
static char fifoBufPop(FifoBuf *buffer);

/**
 * Push character into fifo buffer. If there is no space left, character is
 * discarded and false is returned
 * @param buffer
 * @param a - character to add
 * @return true if char was added to buffer, false otherwise
 */
static bool fifoBufPush(FifoBuf *buffer, char a);

/**
 * Copy provided string to the history buffer.
 * If it is already inside history, it will be removed from it and added again.
 * So after addition, it will always be on top
 * If available size is not enough (and total size is enough) old elements will
 * be removed from history so this item can be put to it
 * @param history
 * @param str
 * @return true if string was put in history
 */
static bool historyPut(CliHistory *history, const char *str);

/**
 * Get item from history. Items are counted from 1 so if item is 0 or greater
 * than itemCount, NULL is returned
 * @param history
 * @param item
 * @return true if string was put in history
 */
static const char *historyGet(CliHistory *history, uint16_t item);

EmbeddedCliConfig *embeddedCliDefaultConfig(void) {
    defaultConfig.rxBufferSize = 64;
    defaultConfig.cmdBufferSize = 64;
    defaultConfig.historyBufferSize = 128;
    defaultConfig.cliBuffer = NULL;
    defaultConfig.cliBufferSize = 0;
    defaultConfig.maxBindingCount = 8;
    return &defaultConfig;
}

uint16_t embeddedCliRequiredSize(EmbeddedCliConfig *config) {
    uint16_t bindingCount = config->maxBindingCount + cliInternalBindingCount;
    return sizeof(EmbeddedCli) + sizeof(EmbeddedCliImpl) +
           config->rxBufferSize * sizeof(char) +
           config->cmdBufferSize * sizeof(char) +
           config->historyBufferSize * sizeof(char) +
           bindingCount * sizeof(CliCommandBinding) +
           bindingCount * sizeof(uint8_t);
}

EmbeddedCli *embeddedCliNew(EmbeddedCliConfig *config) {
    EmbeddedCli *cli = NULL;

    uint16_t bindingCount = config->maxBindingCount + cliInternalBindingCount;

    size_t totalSize = embeddedCliRequiredSize(config);

    bool allocated = false;
    if (config->cliBuffer == NULL) {
        config->cliBuffer = malloc(totalSize);
        if (config->cliBuffer == NULL)
            return NULL;
        allocated = true;
    } else if (config->cliBufferSize < totalSize) {
        return NULL;
    }

    uint8_t *buf = config->cliBuffer;

    memset(buf, 0, totalSize);

    cli = (EmbeddedCli *) buf;
    buf = &buf[sizeof(EmbeddedCli)];

    cli->_impl = (EmbeddedCliImpl *) buf;
    buf = &buf[sizeof(EmbeddedCliImpl)];

    PREPARE_IMPL(cli)
    impl->rxBuffer.buf = (char *) buf;
    buf = &buf[config->rxBufferSize * sizeof(char)];

    impl->cmdBuffer = (char *) buf;
    buf = &buf[config->cmdBufferSize * sizeof(char)];

    impl->bindings = (CliCommandBinding *) buf;
    buf = &buf[bindingCount * sizeof(CliCommandBinding)];

    impl->bindingsFlags = buf;

    if (allocated)
        SET_FLAG(impl->flags, CLI_FLAG_ALLOCATED);

    impl->rxBuffer.size = config->rxBufferSize;
    impl->rxBuffer.front = 0;
    impl->rxBuffer.back = 0;
    impl->cmdMaxSize = config->cmdBufferSize;
    impl->bindingsCount = 0;
    impl->maxBindingsCount = config->maxBindingCount + cliInternalBindingCount;
    impl->lastChar = '\0';
    impl->invitation = "> ";

    initInternalBindings(cli);

    return cli;
}

EmbeddedCli *embeddedCliNewDefault(void) {
    return embeddedCliNew(embeddedCliDefaultConfig());
}

void embeddedCliReceiveChar(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli)

    if (!fifoBufPush(&impl->rxBuffer, c)) {
        SET_FLAG(impl->flags, CLI_FLAG_OVERFLOW);
    }
}

void embeddedCliProcess(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)


    if (!IS_FLAG_SET(impl->flags, CLI_FLAG_INIT_COMPLETE)) {
        SET_FLAG(impl->flags, CLI_FLAG_INIT_COMPLETE);
        writeToOutput(cli, impl->invitation);
    }

    while (fifoBufAvailable(&impl->rxBuffer)) {
        char c = fifoBufPop(&impl->rxBuffer);

        if (IS_FLAG_SET(impl->flags, CLI_FLAG_ESCAPE_MODE)) {
            onEscapedInput(cli, c);
        } else if (impl->lastChar == 0x1B && c == '[') {
            //enter escape mode
            SET_FLAG(impl->flags, CLI_FLAG_ESCAPE_MODE);
        } else if (isControlChar(c)) {
            onControlInput(cli, c);
        } else if (isDisplayableChar(c)) {
            onCharInput(cli, c);
        }

        printLiveAutocompletion(cli);

        impl->lastChar = c;
    }

    // discard unfinished command if overflow happened
    if (IS_FLAG_SET(impl->flags, CLI_FLAG_OVERFLOW)) {
        impl->cmdSize = 0;
        UNSET_FLAG(impl->flags, CLI_FLAG_OVERFLOW);
    }
}

bool embeddedCliAddBinding(EmbeddedCli *cli, CliCommandBinding binding) {
    PREPARE_IMPL(cli)
    if (impl->bindingsCount == impl->maxBindingsCount)
        return false;

    impl->bindings[impl->bindingsCount] = binding;

    ++impl->bindingsCount;
    return true;
}

void embeddedCliPrint(EmbeddedCli *cli, const char *string) {
    PREPARE_IMPL(cli)

    // remove chars for autocompletion and live command
    clearCurrentLine(cli);

    // print provided string
    writeToOutput(cli, string);
    writeToOutput(cli, lineBreak);

    // print current command back to screen
    impl->cmdBuffer[impl->cmdSize] = '\0';
    writeToOutput(cli, impl->invitation);
    writeToOutput(cli, impl->cmdBuffer);
    impl->inputLineLength = impl->cmdSize;

    printLiveAutocompletion(cli);
}

void embeddedCliFree(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)
    if (IS_FLAG_SET(impl->flags, CLI_FLAG_ALLOCATED)) {
        // allocation is done in single call to malloc, so need only single free
        free(cli);
    }
}

void embeddedCliTokenizeArgs(char *args) {
    if (args == NULL)
        return;

    // for now only space, but can add more later
    const char *separators = " ";
    size_t len = strlen(args);
    // place extra null char to indicate end of tokens
    args[len + 1] = '\0';

    if (len == 0)
        return;

    // replace all separators with \0 char
    for (int i = 0; i < len; ++i) {
        if (strchr(separators, args[i]) != NULL) {
            args[i] = '\0';
        }
    }

    // compress all sequential null-chars to single ones, starting from end

    size_t nextTokenStartIndex = 0;
    size_t i = len;
    while (i > 0) {
        --i;
        bool isSeparator = strchr(separators, args[i]) != NULL;

        if (!isSeparator && args[i + 1] == '\0') {
            // found end of token, move tokens on the right side of this one
            if (nextTokenStartIndex != 0 && nextTokenStartIndex - i > 2) {
                // will copy all tokens to the right and two null-chars
                memmove(&args[i + 2], &args[nextTokenStartIndex], len - nextTokenStartIndex + 1);
            }
        } else if (isSeparator && args[i + 1] != '\0') {
            nextTokenStartIndex = i + 1;
        }
    }

    // remove null chars from the beginning
    if (args[0] == '\0' && nextTokenStartIndex > 0) {
        memmove(args, &args[nextTokenStartIndex], len - nextTokenStartIndex + 1);
    }
}

const char *embeddedCliGetToken(const char *tokenizedStr, uint8_t pos) {
    if (tokenizedStr == NULL || pos == 0)
        return NULL;
    int i = 0;
    int tokenCount = 1;
    while (true) {
        if (tokenCount == pos)
            break;

        if (tokenizedStr[i] == '\0') {
            ++tokenCount;
            if (tokenizedStr[i + 1] == '\0')
                break;
        }

        ++i;
    }

    if (tokenizedStr[i] != '\0')
        return &tokenizedStr[i];
    else
        return NULL;
}

uint8_t embeddedCliFindToken(const char *tokenizedStr, const char *token) {
    if (tokenizedStr == NULL || token == NULL)
        return 0;

    uint8_t size = embeddedCliGetTokenCount(tokenizedStr);
    for (int i = 0; i < size; ++i) {
        if (strcmp(embeddedCliGetToken(tokenizedStr, i + 1), token) == 0)
            return i + 1;
    }

    return 0;
}

uint8_t embeddedCliGetTokenCount(const char *tokenizedStr) {
    if (tokenizedStr == NULL || tokenizedStr[0] == '\0')
        return 0;

    int i = 0;
    int tokenCount = 1;
    while (true) {
        if (tokenizedStr[i] == '\0') {
            if (tokenizedStr[i + 1] == '\0')
                break;
            ++tokenCount;
        }
        ++i;
    }

    return tokenCount;
}

static void onEscapedInput(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli)

    if (c >= 64 && c <= 126) {
        // handle escape sequence
        UNSET_FLAG(impl->flags, CLI_FLAG_ESCAPE_MODE);
    }
}

static void onCharInput(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli)

    // have to reserve two extra chars for command ending (used in tokenization)
    if (impl->cmdSize + 2 >= impl->cmdMaxSize)
        return;

    impl->cmdBuffer[impl->cmdSize] = c;
    ++impl->cmdSize;

    if (cli->writeChar != NULL)
        cli->writeChar(cli, c);
}

static void onControlInput(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli)

    // process \r\n and \n\r as single \r\n command
    if ((impl->lastChar == '\r' && c == '\n') ||
        (impl->lastChar == '\n' && c == '\r'))
        return;

    if (c == '\r' || c == '\n') {
        writeToOutput(cli, lineBreak);

        if (impl->cmdSize > 0)
            parseCommand(cli);
        impl->cmdSize = 0;
        impl->inputLineLength = 0;

        writeToOutput(cli, impl->invitation);
    } else if ((c == '\b' || c == 0x7F) && impl->cmdSize > 0) {
        // remove char from screen
        cli->writeChar(cli, '\b');
        cli->writeChar(cli, ' ');
        cli->writeChar(cli, '\b');
        // and from buffer
        --impl->cmdSize;
    } else if (c == '\t') {
        onAutocompleteRequest(cli);
    }

}

static void parseCommand(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)

    char *cmdName = NULL;
    char *cmdArgs = NULL;
    bool nameFinished = false;

    // find command name and command args inside command buffer
    for (int i = 0; i < impl->cmdSize; ++i) {
        char c = impl->cmdBuffer[i];

        if (c == ' ') {
            // all spaces between name and args are filled with zeros
            // so name is a correct null-terminated string
            if (cmdArgs == NULL)
                impl->cmdBuffer[i] = '\0';
            if (cmdName != NULL)
                nameFinished = true;

        } else if (cmdName == NULL) {
            cmdName = &impl->cmdBuffer[i];
        } else if (cmdArgs == NULL && nameFinished) {
            cmdArgs = &impl->cmdBuffer[i];
        }
    }

    // we keep two last bytes in cmd buffer reserved so cmdSize is always by 2
    // less than cmdMaxSize
    impl->cmdBuffer[impl->cmdSize] = '\0';
    impl->cmdBuffer[impl->cmdSize + 1] = '\0';

    if (cmdName == NULL)
        return;

    // try to find command in bindings
    for (int i = 0; i < impl->bindingsCount; ++i) {
        if (strcmp(cmdName, impl->bindings[i].name) == 0) {
            if (impl->bindings[i].binding == NULL)
                break;

            if (impl->bindings[i].tokenizeArgs)
                embeddedCliTokenizeArgs(cmdArgs);
            impl->bindings[i].binding(cli, cmdArgs, impl->bindings[i].context);
            return;
        }
    }

    // command not found in bindings or binding was null
    // try to call default callback
    if (cli->onCommand != NULL) {
        CliCommand command;
        command.name = cmdName;
        command.args = cmdArgs;

        cli->onCommand(cli, &command);
    } else {
        onUnknownCommand(cli, cmdName);
    }
}

static void initInternalBindings(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)

    CliCommandBinding b = {
            "help",
            "Print list of commands",
            true,
            NULL,
            onHelp
    };
    embeddedCliAddBinding(cli, b);
}

static void onHelp(EmbeddedCli *cli, char *tokens, void *context) {
    PREPARE_IMPL(cli)

    if (impl->bindingsCount == 0) {
        writeToOutput(cli, "Help is not available");
        writeToOutput(cli, lineBreak);
        return;
    }

    uint8_t tokenCount = embeddedCliGetTokenCount(tokens);
    if (tokenCount == 0) {
        for (int i = 0; i < impl->bindingsCount; ++i) {
            writeToOutput(cli, " * ");
            writeToOutput(cli, impl->bindings[i].name);
            writeToOutput(cli, lineBreak);
            if (impl->bindings[i].help != NULL) {
                cli->writeChar(cli, '\t');
                writeToOutput(cli, impl->bindings[i].help);
                writeToOutput(cli, lineBreak);
            }
        }
    } else if (tokenCount == 1) {
        // try find command
        const char *helpStr = NULL;
        const char *cmdName = embeddedCliGetToken(tokens, 1);
        bool found = false;
        for (int i = 0; i < impl->bindingsCount; ++i) {
            if (strcmp(impl->bindings[i].name, cmdName) == 0) {
                helpStr = impl->bindings[i].help;
                found = true;
                break;
            }
        }
        if (found && helpStr != NULL) {
            writeToOutput(cli, " * ");
            writeToOutput(cli, cmdName);
            writeToOutput(cli, lineBreak);
            cli->writeChar(cli, '\t');
            writeToOutput(cli, helpStr);
            writeToOutput(cli, lineBreak);
        } else if (found) {
            writeToOutput(cli, "Help is not available");
            writeToOutput(cli, lineBreak);
        } else {
            onUnknownCommand(cli, cmdName);
        }
    } else {
        writeToOutput(cli, "Command \"help\" receives one or zero arguments");
        writeToOutput(cli, lineBreak);
    }
}

static void onUnknownCommand(EmbeddedCli *cli, const char *name) {
    writeToOutput(cli, "Unknown command: \"");
    writeToOutput(cli, name);
    writeToOutput(cli, "\". Write \"help\" for a list of available commands");
    writeToOutput(cli, lineBreak);
}

static AutocompletedCommand getAutocompletedCommand(EmbeddedCli *cli, const char *prefix) {
    AutocompletedCommand cmd = {NULL, 0, 0};

    size_t prefixLen = strlen(prefix);

    PREPARE_IMPL(cli)
    if (impl->bindingsCount == 0 || prefixLen == 0)
        return cmd;


    for (int i = 0; i < impl->bindingsCount; ++i) {
        const char *name = impl->bindings[i].name;
        size_t len = strlen(name);

        // unset autocomplete flag
        impl->bindingsFlags[i] &= ~BINDING_FLAG_AUTOCOMPLETE;

        if (len < prefixLen)
            continue;

        // check if this command is candidate for autocomplete
        bool isCandidate = true;
        for (int j = 0; j < prefixLen; ++j) {
            if (prefix[j] != name[j]) {
                isCandidate = false;
                break;
            }
        }
        if (!isCandidate)
            continue;

        impl->bindingsFlags[i] |= BINDING_FLAG_AUTOCOMPLETE;

        if (cmd.candidateCount == 0 || len < cmd.autocompletedLen)
            cmd.autocompletedLen = len;

        ++cmd.candidateCount;

        if (cmd.candidateCount == 1) {
            cmd.firstCandidate = name;
            continue;
        }

        for (int j = impl->cmdSize; j < cmd.autocompletedLen; ++j) {
            if (cmd.firstCandidate[j] != name[j]) {
                cmd.autocompletedLen = j;
                break;
            }
        }
    }

    return cmd;
}

static void printLiveAutocompletion(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)

    impl->cmdBuffer[impl->cmdSize] = '\0';
    AutocompletedCommand cmd = getAutocompletedCommand(cli, impl->cmdBuffer);

    if (cmd.candidateCount == 0) {
        if (impl->inputLineLength > impl->cmdSize) {
            //TODO can replace with spaces and use \b instead
            clearCurrentLine(cli);
            writeToOutput(cli, impl->invitation);
            writeToOutput(cli, impl->cmdBuffer);
        }
        impl->inputLineLength = impl->cmdSize;
        return;
    }

    // print live autocompletion
    for (int i = impl->cmdSize; i < cmd.autocompletedLen; ++i) {
        cli->writeChar(cli, cmd.firstCandidate[i]);
    }
    // replace with spaces previous autocompletion
    for (int i = cmd.autocompletedLen; i < impl->inputLineLength; ++i) {
        cli->writeChar(cli, ' ');
    }
    impl->inputLineLength = cmd.autocompletedLen;
    cli->writeChar(cli, '\r');
    // print current command again so cursor is moved to initial place
    writeToOutput(cli, impl->invitation);
    writeToOutput(cli, impl->cmdBuffer);

}

static void onAutocompleteRequest(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)

    impl->cmdBuffer[impl->cmdSize] = '\0';
    AutocompletedCommand cmd = getAutocompletedCommand(cli, impl->cmdBuffer);

    if (cmd.candidateCount == 0)
        return;

    if (cmd.candidateCount == 1) {
        // complete command and insert space
        for (int i = impl->cmdSize; i < cmd.autocompletedLen; ++i) {
            char c = cmd.firstCandidate[i];
            cli->writeChar(cli, c);
            impl->cmdBuffer[i] = c;
        }
        cli->writeChar(cli, ' ');
        impl->cmdBuffer[cmd.autocompletedLen] = ' ';
        impl->cmdSize = cmd.autocompletedLen + 1;
        impl->inputLineLength = impl->cmdSize;
        return;
    }
    // cmd.candidateCount > 1

    // with multiple candidates we either complete to common prefix
    // or show all candidates if we already have common prefix
    if (cmd.autocompletedLen == impl->cmdSize) {
        // we need to completely clear current line since it begins with invitation
        clearCurrentLine(cli);

        for (int i = 0; i < impl->bindingsCount; ++i) {
            // autocomplete flag is set for all candidates by last call to
            // getAutocompletedCommand
            if (!(impl->bindingsFlags[i] & BINDING_FLAG_AUTOCOMPLETE))
                continue;

            const char *name = impl->bindings[i].name;

            writeToOutput(cli, name);
            writeToOutput(cli, lineBreak);
        }

        impl->cmdBuffer[impl->cmdSize] = '\0';
        writeToOutput(cli, impl->invitation);
        writeToOutput(cli, impl->cmdBuffer);
    } else {
        // complete to common prefix
        for (int i = impl->cmdSize; i < cmd.autocompletedLen; ++i) {
            char c = cmd.firstCandidate[i];
            cli->writeChar(cli, c);
            impl->cmdBuffer[i] = c;
        }
        impl->cmdSize = cmd.autocompletedLen;
    }
    impl->inputLineLength = impl->cmdSize;
}

static void clearCurrentLine(EmbeddedCli *cli) {
    PREPARE_IMPL(cli)
    size_t len = impl->inputLineLength + strlen(impl->invitation);

    cli->writeChar(cli, '\r');
    for (int i = 0; i < len; ++i) {
        cli->writeChar(cli, ' ');
    }
    cli->writeChar(cli, '\r');
    impl->inputLineLength = 0;
}

static void writeToOutput(EmbeddedCli *cli, const char *str) {
    size_t len = strlen(str);

    for (int i = 0; i < len; ++i) {
        cli->writeChar(cli, str[i]);
    }
}

static bool isControlChar(char c) {
    return c == '\r' || c == '\n' || c == '\b' || c == '\t' || c == 0x7F;
}

static bool isDisplayableChar(char c) {
    return (c >= 32 && c <= 126);
}

static uint16_t fifoBufAvailable(FifoBuf *buffer) {
    if (buffer->back >= buffer->front)
        return buffer->back - buffer->front;
    else
        return buffer->size - buffer->front + buffer->back;
}

static char fifoBufPop(FifoBuf *buffer) {
    char a = '\0';
    if (buffer->front != buffer->back) {
        a = buffer->buf[buffer->front];
        buffer->front = (buffer->front + 1) % buffer->size;
    }
    return a;
}

static bool fifoBufPush(FifoBuf *buffer, char a) {
    uint32_t newBack = (buffer->back + 1) % buffer->size;
    if (newBack != buffer->front) {
        buffer->buf[buffer->back] = a;
        buffer->back = newBack;
        return true;
    }
    return false;
}

static bool historyPut(CliHistory *history, const char *str) {
    size_t len = strlen(str);
    // each item is ended with \0 so, need to have that much space at least
    if (history->bufferSize < len + 1)
        return false;

    size_t usedSize;
    // remove old items if new one can't fit into buffer
    while (true) {
        const char *item = historyGet(history, history->itemsCount);
        size_t itemLen = strlen(item);
        usedSize = (item - history->buf) + itemLen + 1;

        size_t freeSpace = history->bufferSize - usedSize;

        if (freeSpace >= len + 1)
            break;

        // space not enough, remove last element
        --history->itemsCount;
    }
    if (history->itemsCount > 0) {
        // when history not empty, shift elements so new item is first
        memmove(&history->buf[len + 1], history->buf, usedSize);
    }
    memcpy(history->buf, str, len + 1);

    return true;
}

static const char *historyGet(CliHistory *history, uint16_t item) {
    if (item == 0 || item > history->itemsCount)
        return NULL;

    // items are stored in the same way (separated by \0 and counted from 1),
    // so can use this call
    return embeddedCliGetToken(history->buf, item);
}
