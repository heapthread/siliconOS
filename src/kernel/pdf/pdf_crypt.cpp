// silicon - PDF: standard security handler (empty user password)
#include "pdf_int.h"

bool pdf_setup_encryption(PdfDoc* d) { return false; }
void pdf_decrypt_obj(PdfDoc* d, Obj* o, u32 num, u32 gen) {}
u8* pdf_decrypt_stream(PdfDoc* d, u8* data, u32* len, u32 num, u32 gen) { return data; }
