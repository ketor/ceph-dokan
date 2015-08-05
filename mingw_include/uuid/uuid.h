#undef uuid_t
typedef unsigned char uuid_t[16];

int uuid_parse(const char *in, uuid_t uu);
void uuid_unparse(const uuid_t uu, char *out);
int uuid_compare(const uuid_t uu1, const uuid_t uu2);
int uuid_is_null(const uuid_t uu);

#define uuid_generate(uuid) return

