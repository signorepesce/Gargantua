#ifndef GARGANTUA_EMIT_INTERNAL_H
#define GARGANTUA_EMIT_INTERNAL_H

void emit_crud(FILE *out, const ParsedType *t);

void emit_crud_declarations(FILE *out, const ParsedType *t);

void emit_fields(FILE *out, const ParsedType *type);

void emit_route_table(FILE *out, const Generator *ctx);

void emit_route_wrappers(FILE *out, const Generator *ctx);

void emit_store_declarations(FILE *out, const Generator *ctx);

void emit_stores(FILE *out, const Generator *ctx);

void emit_task_table(FILE *out, const Generator *ctx);

void emit_type(FILE *out, const ParsedType *type);

#endif
