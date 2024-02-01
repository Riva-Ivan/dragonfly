%top{
  // TOP SECTION
  namespace dfly::json {
    struct Parser {
        using symbol_type = int;
    };
  }
}


%{
  // SECOND SECTION
%}

%o bison-cc-namespace="dfly.json" bison-cc-parser="Parser"
%o namespace="dfly.json"
%o class="Lexer" lex="Lex"
%o nodefault batch


/* Declarations before lexer implementation.  */
%{
    // Declarations
%}


%{
  // Code run each time a pattern is matched.
%}

%%

%{
  // Code run each time lex() is called.
%}

[[:space:]]+     ; // skip white space


<<EOF>>    printf("EOF%s\n", matcher().text());
%%

// Function definitions
