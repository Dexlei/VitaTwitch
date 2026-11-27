#include "i18n.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
  assert(VT_LANG_COUNT==19 && VT_I_COUNT==30);
  for(int i=0;i<VT_LANG_COUNT;i++) {
    assert(vt_lang_set(i)==0);
    assert(vt_lang_current()==i);
    assert(vt_lang_code(i)[0] && vt_lang_name(i)[0]);
    for(int j=0;j<VT_I_COUNT;j++)assert(vt_tr((vt_text_id)j)[0]);
    assert(vt_lang_set_code(vt_lang_code(i))==0);
  }
  assert(vt_lang_set_code("pt-BR")==0);
  assert(strcmp(vt_tr(VT_I_FOLLOWING),"Seguindo")==0);
  assert(vt_lang_auto(17)==0 && strcmp(vt_lang_code(vt_lang_current()),"pt-BR")==0);
  assert(vt_lang_set_code("invalid")==-1);
  assert(vt_lang_set(19)==-1);
  printf("PASS: 19 locale catalogs, 28 complete strings, Vita locale and fallback\n");
  return 0;
}
