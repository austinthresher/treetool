#ifndef X
#error  "To use this file, define X as a function-like macro " \
	"with two arguments, then include this. The first argument " \
	"is the enum constant and the second is the description string. "
#endif

/* enum           description        */

X( NONE,         "Nothing"           )
X( NULLARG,      "Null Argument: "   )
X( FILENOTFOUND, "File Not Found: "  )
X( IO,           "IO Error: "        )
X( FORMAT,       "Format Error: "    )
X( RUNTIME,      "Runtime Error: "   )
X( ALLOC,        "Allocation Error: ")

#undef X
