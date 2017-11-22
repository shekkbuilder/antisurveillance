int Scripting_Perform(AS_context *ctx);
int Scripting_Init(AS_context *ctx);

AS_scripts *Scripting_New(AS_context *ctx);



// scripts that are loaded that should be checked periodically for control variables, etc
typedef struct _as_scripts {
    struct _as_scripts *next;

    int id;

    // type of script? python, etc...
    int type;
 
    AS_context *ctx;

    // scripts custom context (like PythonModuleCustom)
    void *script_context;
} AS_scripts;

