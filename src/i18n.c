#include "i18n.h"
#include <string.h>
/* Nineteen translated navigation/menu/chat strings; legacy errors may still
 * appear in Spanish until the complete diagnostic UI is localized.
 * UTF-8 labels are data: font glyph availability depends on Vita firmware. */
typedef struct {const char *code, *name; const char *text[VT_I_COUNT];} lang_t;
static const lang_t langs[VT_LANG_COUNT] = {
  {"en", "English", {"Following", "Explore", "Search", "LIVE", "Offline", "Menu", "Account", "Language", "Touch", "Chat", "Write", "Log in to send messages", "On", "Off", "Refresh", "Close", "Play", "Quality", "Loading", "Back", "Exit", "Settings", "Clear thumbnails", "About", "Log in", "Log out", "Tab", "Send message", "Show info", "Hide info"}},
  {"es", "Español", {"Siguiendo", "Explorar", "Buscar", "DIRECTO", "Desconectado", "Menú", "Cuenta", "Idioma", "Táctil", "Chat", "Escribir", "Inicia sesión para escribir", "Activado", "Desactivado", "Actualizar", "Cerrar", "Reproducir", "Calidad", "Cargando", "Volver", "Salir", "Ajustes", "Limpiar miniaturas", "Acerca de", "Iniciar sesión", "Cerrar sesión", "Pestaña", "Enviar mensaje", "Mostrar info", "Ocultar info"}},
  {"pt-BR", "Português (Brasil)", {"Seguindo", "Explorar", "Buscar", "AO VIVO", "Offline", "Menu", "Conta", "Idioma", "Toque", "Chat", "Escrever", "Entre na conta para enviar", "Ativado", "Desativado", "Atualizar", "Fechar", "Reproduzir", "Qualidade", "Carregando", "Voltar", "Sair", "Configurações", "Limpar miniaturas", "Sobre", "Entrar", "Sair da conta", "Aba", "Enviar mensagem", "Mostrar informações", "Ocultar informações"}},
  {"fr", "Français", {"Abonnements", "Explorer", "Rechercher", "EN DIRECT", "Hors ligne", "Menu", "Compte", "Langue", "Tactile", "Chat", "Écrire", "Connectez-vous pour écrire", "Activé", "Désactivé", "Actualiser", "Fermer", "Lire", "Qualité", "Chargement", "Retour", "Quitter", "Paramètres", "Vider les vignettes", "À propos", "Connexion", "Déconnexion", "Onglet", "Envoyer un message", "Afficher infos", "Masquer infos"}},
  {"it", "Italiano", {"Seguiti", "Esplora", "Cerca", "IN DIRETTA", "Offline", "Menu", "Account", "Lingua", "Touch", "Chat", "Scrivi", "Accedi per scrivere", "Attivo", "Disattivo", "Aggiorna", "Chiudi", "Riproduci", "Qualità", "Caricamento", "Indietro", "Esci", "Impostazioni", "Pulisci miniature", "Info", "Accedi", "Esci dall'account", "Scheda", "Invia messaggio", "Mostra info", "Nascondi info"}},
  {"de", "Deutsch", {"Gefolgt", "Entdecken", "Suchen", "LIVE", "Offline", "Menü", "Konto", "Sprache", "Touch", "Chat", "Schreiben", "Zum Chatten anmelden", "Ein", "Aus", "Aktualisieren", "Schließen", "Abspielen", "Qualität", "Laden", "Zurück", "Beenden", "Einstellungen", "Vorschaubilder leeren", "Info", "Anmelden", "Abmelden", "Register", "Nachricht senden", "Info anzeigen", "Info ausblenden"}},
  {"ru", "Русский", {"Подписки", "Обзор", "Поиск", "ЭФИР", "Не в сети", "Меню", "Аккаунт", "Язык", "Касание", "Чат", "Написать", "Войдите, чтобы писать", "Вкл", "Выкл", "Обновить", "Закрыть", "Смотреть", "Качество", "Загрузка", "Назад", "Выход", "Настройки", "Очистить превью", "О программе", "Войти", "Выйти", "Вкладка", "Отправить сообщение", "Показать инфо", "Скрыть инфо"}},
  {"id", "Bahasa Indonesia", {"Mengikuti", "Jelajahi", "Cari", "LIVE", "Offline", "Menu", "Akun", "Bahasa", "Sentuh", "Obrolan", "Tulis", "Masuk untuk mengirim pesan", "Aktif", "Nonaktif", "Segarkan", "Tutup", "Putar", "Kualitas", "Memuat", "Kembali", "Keluar", "Pengaturan", "Hapus thumbnail", "Tentang", "Masuk", "Keluar akun", "Tab", "Kirim pesan", "Tampilkan info", "Sembunyikan info"}},
  {"fil", "Filipino", {"Sinusundan", "Tuklasin", "Hanapin", "LIVE", "Offline", "Menu", "Account", "Wika", "Pindot", "Chat", "Magsulat", "Mag-login para magpadala", "Bukas", "Patay", "I-refresh", "Isara", "I-play", "Kalidad", "Naglo-load", "Bumalik", "Lumabas", "Settings", "Linisin ang larawan", "Tungkol", "Mag-login", "Mag-logout", "Tab", "Magpadala ng mensahe", "Ipakita ang info", "Itago ang info"}},
  {"ceb", "Cebuano", {"Gisundan", "Susiha", "Pangitaa", "LIVE", "Offline", "Menu", "Account", "Pinulongan", "Hikap", "Chat", "Pagsulat", "Pag-login aron makapadala", "On", "Off", "I-refresh", "Sirad-i", "I-play", "Kalidad", "Nag-load", "Balik", "Gawas", "Settings", "Hawani ang hulagway", "Mahitungod", "Pag-login", "Pag-logout", "Tab", "Padala og mensahe", "Ipakita ang info", "Tagoa ang info"}},
  {"nl", "Nederlands", {"Volgend", "Ontdekken", "Zoeken", "LIVE", "Offline", "Menu", "Account", "Taal", "Aanraken", "Chat", "Schrijven", "Log in om te chatten", "Aan", "Uit", "Vernieuwen", "Sluiten", "Afspelen", "Kwaliteit", "Laden", "Terug", "Afsluiten", "Instellingen", "Miniaturen wissen", "Over", "Inloggen", "Uitloggen", "Tab", "Bericht sturen", "Info tonen", "Info verbergen"}},
  {"pl", "Polski", {"Obserwowani", "Odkrywaj", "Szukaj", "NA ŻYWO", "Offline", "Menu", "Konto", "Język", "Dotyk", "Czat", "Napisz", "Zaloguj się, aby pisać", "Wł.", "Wył.", "Odśwież", "Zamknij", "Odtwórz", "Jakość", "Ładowanie", "Wstecz", "Wyjdź", "Ustawienia", "Wyczyść miniatury", "O aplikacji", "Zaloguj", "Wyloguj", "Karta", "Wyślij wiadomość", "Pokaż informacje", "Ukryj informacje"}},
  {"tr", "Türkçe", {"Takip edilenler", "Keşfet", "Ara", "CANLI", "Çevrimdışı", "Menü", "Hesap", "Dil", "Dokunmatik", "Sohbet", "Yaz", "Yazmak için giriş yapın", "Açık", "Kapalı", "Yenile", "Kapat", "Oynat", "Kalite", "Yükleniyor", "Geri", "Çıkış", "Ayarlar", "Küçük resimleri temizle", "Hakkında", "Giriş yap", "Çıkış yap", "Sekme", "Mesaj gönder", "Bilgiyi göster", "Bilgiyi gizle"}},
  {"uk", "Українська", {"Підписки", "Огляд", "Пошук", "НАЖИВО", "Не в мережі", "Меню", "Обліковий запис", "Мова", "Дотик", "Чат", "Написати", "Увійдіть, щоб писати", "Увімк.", "Вимк.", "Оновити", "Закрити", "Дивитися", "Якість", "Завантаження", "Назад", "Вийти", "Налаштування", "Очистити прев'ю", "Про програму", "Увійти", "Вийти з акаунта", "Вкладка", "Надіслати повідомлення", "Показати інфо", "Сховати інфо"}},
  {"sv", "Svenska", {"Följer", "Utforska", "Sök", "LIVE", "Offline", "Meny", "Konto", "Språk", "Pekskärm", "Chatt", "Skriv", "Logga in för att skriva", "På", "Av", "Uppdatera", "Stäng", "Spela", "Kvalitet", "Laddar", "Tillbaka", "Avsluta", "Inställningar", "Rensa miniatyrer", "Om", "Logga in", "Logga ut", "Flik", "Skicka meddelande", "Visa info", "Dölj info"}},
  {"ja", "Japanese (日本語)", {"フォロー中", "探す", "検索", "ライブ", "オフライン", "メニュー", "アカウント", "言語", "タッチ", "チャット", "入力", "送信にはログインが必要", "オン", "オフ", "更新", "閉じる", "再生", "画質", "読込中", "戻る", "終了", "設定", "画像キャッシュ削除", "情報", "ログイン", "ログアウト", "タブ", "メッセージ送信", "情報を表示", "情報を隠す"}},
  {"ko", "Korean (한국어)", {"팔로잉", "탐색", "검색", "생방송", "오프라인", "메뉴", "계정", "언어", "터치", "채팅", "입력", "메시지를 보내려면 로그인", "켜기", "끄기", "새로고침", "닫기", "재생", "화질", "로딩 중", "뒤로", "종료", "설정", "썸네일 삭제", "정보", "로그인", "로그아웃", "탭", "메시지 보내기", "정보 보기", "정보 숨기기"}},
  {"zh-CN", "Chinese Simplified (简体中文)", {"关注", "发现", "搜索", "直播", "离线", "菜单", "账户", "语言", "触摸", "聊天", "输入", "登录后才能发送", "开启", "关闭", "刷新", "关闭", "播放", "画质", "加载中", "返回", "退出", "设置", "清理缩略图", "关于", "登录", "退出登录", "标签", "发送消息", "显示信息", "隐藏信息"}},
  {"zh-TW", "Chinese Traditional (繁體中文)", {"追蹤", "探索", "搜尋", "直播", "離線", "選單", "帳戶", "語言", "觸控", "聊天", "輸入", "登入後才能傳送", "開啟", "關閉", "重新整理", "關閉", "播放", "畫質", "載入中", "返回", "退出", "設定", "清除縮圖", "關於", "登入", "登出", "分頁", "傳送訊息", "顯示資訊", "隱藏資訊"}},
};
static int active = 1; /* existing default: Spanish */
int vt_lang_current(void) {return active;}
int vt_lang_set(int index) {if(index<0||index>=VT_LANG_COUNT)return -1;active=index;return 0;}
int vt_lang_set_code(const char *code) {
  if(!code)return -1;
  for(int i=0;i<VT_LANG_COUNT;i++)if(strcmp(code,langs[i].code)==0)return vt_lang_set(i);
  return -1;
}
const char *vt_lang_code(int i) {return i>=0&&i<VT_LANG_COUNT?langs[i].code:"en";}
const char *vt_lang_name(int i) {return i>=0&&i<VT_LANG_COUNT?langs[i].name:"English";}
const char *vt_tr(vt_text_id id) {
  if(id<0||id>=VT_I_COUNT)return "";
  const char *v=langs[active].text[id];
  return v&&*v?v:langs[0].text[id];
}
int vt_lang_auto(int sys) {
  /* SceSystemParamLang values; this is pure C to retain host testability.
   * Order: ja en fr es de it nl pt-PT ru ko zh-TW zh-CN fi sv da no pl pt-BR en-GB tr. */
  const int mapping[]={15,0,3,1,5,4,10,2,6,16,18,17,0,14,0,0,11,2,0,12};
  if(sys<0||sys>=(int)(sizeof(mapping)/sizeof(mapping[0])))return -1;
  return vt_lang_set(mapping[sys]);
}
