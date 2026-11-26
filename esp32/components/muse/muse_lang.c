/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_lang.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "muse_text.h"
#include "sdkconfig.h"
#if !CONFIG_MUSE_UI_FREEINK
#include "lvgl.h"
#endif

/*
 * A translation is looked up by its English text, so a caption the code
 * changes falls back to English rather than to the wrong words.
 * test_muse_lang.py checks that each English text is still in the code, that
 * a translation keeps its format conversions and status names the English's
 * case, and that every letter is one its fonts have. A language adds its
 * tables, the Montserrat sizes its letters need and a Kconfig choice. The
 * touch boards' settings pages (muse_settings_ui.c) are translated in
 * Brazilian Portuguese only; their text goes through muse_lang_menu.
 */
typedef struct {
    const char *en, *text;
} muse_lang_entry_t;

#if CONFIG_MUSE_UI_LANG_VI || CONFIG_MUSE_UI_LANG_PT_BR
#if CONFIG_MUSE_UI_LANG_VI
/* Short names, in sentence case like the English: they head the
 * screen. A turn's listening, thinking and speaking show no name.
 * READY: muse_ui.c sizes the state row with it. */
static const muse_lang_entry_t STATUS[] = {
    { "Waking up", "Đang khởi động" },
    { "READY", "SẴN SÀNG" },
    { "Ready", "Sẵn sàng" },
    { "Error", "Lỗi" },
    { "Goodbye", "Tạm biệt" },
    { "Wi-Fi off", "Wi-Fi đang tắt" },
    { "Set up Wi-Fi", "Chưa cài Wi-Fi" },
    { "No Wi-Fi", "Không có Wi-Fi" },
    { "Reconnecting", "Đang kết nối lại" },
    { "Connecting", "Đang kết nối" },
    { "USB power", "Nguồn USB" },
    { "Charging %d%%", "Đang sạc %d%%" },
    { "Battery %d%%", "Pin %d%%" },
};

/* Sentence case: a capital with two marks (Ấ, Ễ) is cramped in 16 px. */
static const muse_lang_entry_t MESSAGES[] = {
    /* The voice path (muse_voice.c, muse_app.c) */
    { "Waking up...", "Đang khởi động..." },
    { "Recording...", "Đang ghi âm..." },
    { "Recording %.1fs", "Đang ghi âm %.1fs" },
    { "Sending voice note", "Đang gửi tin nhắn thoại" },
    { "Note sent, waiting for Muse", "Đã gửi, đang chờ Muse" },
    { "Hold to talk", "Giữ nút lâu hơn để nói" },   /* a press too short to count */
    { "Record a longer note", "Hãy ghi âm lâu hơn" },
    { "Microphone error, try again", "Lỗi mic - thử lại" },
    { "Set up Muse first", "Hãy cài đặt Muse trước" },
    { "Not now", "Để sau" },   /* a re-setup's prompt dismissed */
    { "No Wi-Fi", "Không có Wi-Fi" },
    { "Can't reach Muse", "Không kết nối được Muse" },
    { "Didn't catch that", "Chưa nghe rõ, nói lại nhé" },
    { "Audio didn't start", "Lỗi khởi động âm thanh" },
    /* Notes saved to send later */
    { "Couldn't save the note", "Không lưu được tin nhắn" },
    { "Saved, will try again", "Đã lưu, sẽ thử lại" },
    { "Saved, sends when online", "Đã lưu, sẽ gửi khi có mạng" },
    { "Sending saved note", "Đang gửi tin nhắn đã lưu" },
    { "Saved note sent", "Đã gửi tin nhắn đã lưu" },
    { "COULDN'T SEND A SAVED NOTE", "Không gửi được tin nhắn đã lưu" },
    { "Saved note: will try again", "Tin nhắn đã lưu: sẽ thử lại" },
    { "Notes still waiting to send", "Còn tin nhắn chờ gửi" },
    { "MAY HAVE SENT - SEE MUSE APP", "Có thể đã gửi: xem app Muse" },
    /* A turn that failed (Muse's own errors, muse_chat*.c) */
    { "CAN'T REACH MUSE", "Không kết nối được Muse" },
    { "MUSE NOT SET UP", "Chưa cài đặt Muse" },
    { "LOST CONNECTION TO MUSE", "Mất kết nối với Muse" },
    { "NO REPLY FROM MUSE", "Muse chưa trả lời" },
    { "DIDN'T CATCH THAT", "Chưa nghe rõ, nói lại nhé" },
    { "MUSE STOPPED LISTENING", "Muse đã ngừng nghe" },
    { "MUSE COULDN'T LISTEN", "Muse không nghe được" },
    { "MUSE DIDN'T TAKE IT", "Muse không nhận tin nhắn" },
    { "INTERRUPTED", "Đã bị ngắt" },
    { "CANCELLED", "Đã huỷ" },
    { "SETTINGS CHANGED", "Đã đổi cài đặt" },
    { "REPLY TOO LONG", "Câu trả lời quá dài" },
    { "REPLY TOO LONG: SEE MUSE APP", "Trả lời quá dài: xem app Muse" },
    { "REPLY BUFFER LIMIT - TRY AGAIN", "Bộ nhớ đệm đầy, thử lại nhé" },
    { "MUSE REPLY ACCESS DENIED (403)", "Muse từ chối truy cập (403)" },
    { "MUSE REPLY AUTH REQUIRED (401)", "Muse cần đăng nhập lại (401)" },
    { "CAN'T SUBSCRIBE TO MUSE", "Không nhận được tin từ Muse" },
    { "CAN'T KEEP UP", "Xử lý không kịp" },
    { "OUT OF MEMORY", "Hết bộ nhớ" },
    /* Buttons and power */
    { "Speaker on", "Đã bật loa" },
    { "Speaker off", "Đã tắt loa" },
    { "Hold to mute", "Giữ để tắt tiếng" },
    { "Hold to unmute", "Giữ để bật tiếng" },
    { "Hold to power off", "Giữ để tắt máy" },
    { "Goodbye!", "Tạm biệt!" },
    { "Couldn't power off", "Không tắt được máy" },
    { "Phone setup on", "Đã bật cài đặt qua điện thoại" },
    { "Phone setup: %s", "Cài qua ĐT: %s" },
    { "Phone setup off", "Đã tắt cài đặt qua điện thoại" },
    { "Resetting...", "Đang đặt lại..." },
    { "RESTARTING...", "Đang khởi động lại..." },
};

/* The button menu (muse_menu.c) and the pairing card (muse_ui.c): sentence
 * case, in Montserrat, but for the titles and pages, drawn in unscii, whose
 * accented letters come from the caption font. */
static const muse_lang_entry_t MENU[] = {
    /* Rows */
    { "Volume", "Âm lượng" },
    { "Speaker", "Loa" },
    { "Brightness", "Độ sáng" },
    { "Mic gain", "Độ nhạy mic" },
    { "Auto-sleep", "Tự ngủ" },
    { "Text size", "Cỡ chữ" },
    { "Phone setup", "Cài qua điện thoại" },
    { "Status", "Trạng thái" },
    { "Battery", "Pin" },
    { "Reset pairing", "Đặt lại ghép nối" },
    { "Screen off", "Tắt màn hình" },
    { "Power off", "Tắt máy" },
    { "Close menu", "Đóng menu" },
    /* What the talk button does */
    { "Change", "Đổi" },
    { "Toggle", "Bật/tắt" },
    { "Open", "Mở" },
    { "Select", "Chọn" },
    { "Close", "Đóng" },
    { "Back", "Quay lại" },
    { "Reset", "Đặt lại" },
    { "Cancel", "Huỷ" },
    { "Esc Back", "Esc Quay lại" },
    { "Esc Cancel", "Esc Huỷ" },
    { LV_SYMBOL_DOWN " Down", LV_SYMBOL_DOWN " Xuống" },
    { "Down " LV_SYMBOL_RIGHT, "Xuống " LV_SYMBOL_RIGHT },
    /* Values */
    { "On", "Bật" },
    { "Off", "Tắt" },
    { "Not set", "Chưa cài" },
    { "Joining", "Đang vào" },
    { "Failed", "Lỗi" },
    { "Not found", "Không thấy" },
    { "Never", "Không bao giờ" },
    { "30 s", "30 giây" },
    { "1 min", "1 phút" },
    { "2 min", "2 phút" },
    { "5 min", "5 phút" },
    { "10 min", "10 phút" },
    { "Small", "Nhỏ" },
    { "Medium", "Vừa" },
    { "Large", "Lớn" },
    { "Extra small", "Rất nhỏ" },
    { "Extra large", "Rất lớn" },
    { "Custom", "Tuỳ chỉnh" },
    /* Titles: the menu's, upper case; the others are their rows' names above */
    { "MENU", "CÀI ĐẶT" },
    { "MENU  ^v Move  <> Change", "CÀI ĐẶT  ^v Chọn  <> Đổi" },
    /* Pages, their columns lined up as in the English */
    { "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nPhone %s\nPower %s\nVer   %s",
      "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nĐT    %s\nNguồn %s\nBản   %s" },
    { "off", "tắt" },
    { "offline", "mất mạng" },
    { "Connected", "Đã kết nối" },
    { "Starting", "Đang khởi động" },
    { "Ready to pair", "Chờ ghép nối" },
    { "App connected", "App đã kết nối" },
    { "Confirm pairing", "Xác nhận ghép nối" },
    { "Connecting", "Đang kết nối" },
    { "Online", "Trực tuyến" },
    { "Offline", "Ngoại tuyến" },
    { "Error", "Lỗi" },
    { "Not set up", "Chưa cài" },
    { "Saved", "Đã lưu" },
    { "Can't connect", "Không kết nối được" },
    { "Unplug USB to\nmeasure how\nlong the\nbattery lasts.", "Rút USB để\nđo xem pin\ndùng được\nbao lâu." },
    { "%s %s\nBatt  %d>%d%%\nRate  %s\nFull  %s\nOff   %s\nSleep %s\nWakes %s\nBusy  %s",
      "%s %s\nPin   %d>%d%%\nHao   %s\nĐầy   %s\nTắt   %s\nNgủ   %s\nThức  %s\nBận   %s" },
    { "On batt", "Dùng pin" },
    { "Last run", "Lần trước" },
    { "Turn Muse off?\n\nPress the %s button to turn it back on.", "Tắt Muse?\n\nBấm nút %s để bật lại." },
    { "Forget Wi-Fi and the Muse app pairing, then restart?", "Quên Wi-Fi và ghép nối app Muse, rồi khởi động lại?" },
    /* The pairing card */
    { "Pairing code", "Mã ghép nối" },
    { "Enter on phone", "Nhập trên điện thoại" },
    { "Enter it on your phone", "Nhập mã trên điện thoại" },
    { "Press", "Bấm" },
    { "Press button", "Bấm nút" },
    { "%s button", "Nút %s" },
    { "Press the %s button", "Bấm nút %s" },
    { "Muse app", "App Muse" },
    { "Pair with Muse", "Ghép nối với Muse" },
    { "Confirm", "Xác nhận" },
    { "Tap Confirm to pair", "Chạm Xác nhận để ghép nối" },
};

#elif CONFIG_MUSE_UI_LANG_PT_BR
/* Short names, in sentence case like the English (READY: muse_ui.c sizes
 * the state row with it): they head the screen. No accents, so the unscii
 * status font draws them as it is. */
static const muse_lang_entry_t STATUS[] = {
    { "Waking up", "Iniciando" },
    { "READY", "PRONTO" },
    { "Ready", "Pronto" },
    { "Error", "Erro" },
    { "Goodbye", "Tchau" },
    { "Wi-Fi off", "Wi-Fi desligado" },
    { "Set up Wi-Fi", "Configure o Wi-Fi" },
    { "No Wi-Fi", "Sem Wi-Fi" },
    { "Reconnecting", "Reconectando" },
    { "Connecting", "Conectando" },
    { "USB power", "Energia USB" },
    { "Charging %d%%", "Carregando %d%%" },
    { "Battery %d%%", "Bateria %d%%" },
};

/* Upper case like the English; accented capitals come from the caption
 * font (CONFIG_MUSE_LATIN_FONT). */
static const muse_lang_entry_t MESSAGES[] = {
    /* The voice path */
    { "Waking up...", "INICIANDO..." },
    { "LISTENING...", "OUVINDO..." },
    { "Recording...", "GRAVANDO..." },
    { "LISTENING %.1fs", "OUVINDO %.1fs" },
    { "Recording %.1fs", "GRAVANDO %.1fs" },
    { "Sending voice note", "ENVIANDO ÁUDIO" },
    { "Note sent, waiting for Muse", "ENVIADO - AGUARDANDO O MUSE" },
    { "Hold to talk", "SEGURE MAIS PARA FALAR" },
    { "Record a longer note", "GRAVE UM ÁUDIO MAIS LONGO" },
    { "Microphone error, try again", "ERRO NO MICROFONE - TENTE DE NOVO" },
    { "SET UP MUSE FIRST", "CONFIGURE O MUSE PRIMEIRO" },
    { "Set up Muse first", "CONFIGURE O MUSE PRIMEIRO" },
    { "Not now", "AGORA NÃO" },
    { "No Wi-Fi", "SEM WI-FI" },
    { "Audio didn't start", "FALHA AO INICIAR O ÁUDIO" },
    { "Muse has an answer", "O MUSE TEM UMA RESPOSTA" },
    { "No reply yet", "AINDA SEM RESPOSTA" },
    { "One moment...", "UM MOMENTO..." },
    { "Input lost, press again", "ENTRADA PERDIDA - APERTE DE NOVO" },
    { "Mic buffer full, release and retry", "MIC CHEIO - SOLTE E TENTE DE NOVO" },
    { "Reply missed, hold to try again", "RESPOSTA PERDIDA - SEGURE E REPITA" },
    /* Notes saved to send later */
    { "Couldn't save the note", "NÃO DEU PARA SALVAR O ÁUDIO" },
    { "Saved, will try again", "SALVO, VOU TENTAR DE NOVO" },
    { "Saved, sends when online", "SALVO, ENVIA QUANDO TIVER REDE" },
    { "Sending saved note", "ENVIANDO ÁUDIO SALVO" },
    { "Saved note sent", "ÁUDIO SALVO ENVIADO" },
    { "COULDN'T SEND A SAVED NOTE", "NÃO DEU PARA ENVIAR O ÁUDIO SALVO" },
    { "Saved note: will try again", "ÁUDIO SALVO: VOU TENTAR DE NOVO" },
    { "Notes still waiting to send", "ÁUDIOS AINDA AGUARDANDO ENVIO" },
    { "MAY HAVE SENT - SEE MUSE APP", "PODE TER SIDO ENVIADO: VEJA NO APP" },
    /* A turn that failed */
    { "CAN'T REACH MUSE", "SEM CONEXÃO COM O MUSE" },
    { "Can't reach Muse", "SEM CONEXÃO COM O MUSE" },
    { "MUSE NOT SET UP", "MUSE NÃO CONFIGURADO" },
    { "LOST CONNECTION TO MUSE", "CONEXÃO COM O MUSE PERDIDA" },
    { "NO REPLY FROM MUSE", "O MUSE NÃO RESPONDEU" },
    { "DIDN'T CATCH THAT", "NÃO ENTENDI, FALE DE NOVO" },
    { "Didn't catch that", "NÃO ENTENDI, FALE DE NOVO" },
    { "MUSE STOPPED LISTENING", "O MUSE PAROU DE OUVIR" },
    { "MUSE COULDN'T LISTEN", "O MUSE NÃO CONSEGUIU OUVIR" },
    { "MUSE DIDN'T TAKE IT", "O MUSE NÃO RECEBEU" },
    { "INTERRUPTED", "INTERROMPIDO" },
    { "CANCELLED", "CANCELADO" },
    { "SETTINGS CHANGED", "CONFIGURAÇÕES ALTERADAS" },
    { "REPLY TOO LONG", "RESPOSTA LONGA DEMAIS" },
    { "REPLY TOO LONG: SEE MUSE APP", "RESPOSTA LONGA: VEJA NO APP" },
    { "REPLY BUFFER LIMIT - TRY AGAIN", "LIMITE DE RESPOSTA - TENTE DE NOVO" },
    { "MUSE REPLY ACCESS DENIED (403)", "ACESSO NEGADO PELO MUSE (403)" },
    { "MUSE REPLY AUTH REQUIRED (401)", "O MUSE PEDIU LOGIN (401)" },
    { "CAN'T SUBSCRIBE TO MUSE", "SEM RECEBER MENSAGENS DO MUSE" },
    { "CAN'T KEEP UP", "NÃO ESTOU DANDO CONTA" },
    { "OUT OF MEMORY", "SEM MEMÓRIA" },
    /* Live calls */
    { "Connecting to Muse...", "CONECTANDO AO MUSE..." },
    { "Hanging up...", "DESLIGANDO..." },
    { "Hold yellow to talk", "SEGURE O AMARELO PARA FALAR" },
    { "In a call, press to hang up", "EM CHAMADA - APERTE P/ DESLIGAR" },
    /* Buttons and power */
    { "Speaker on", "ALTO-FALANTE LIGADO" },
    { "Speaker off", "ALTO-FALANTE DESLIGADO" },
    { "Hold to mute", "SEGURE PARA SILENCIAR" },
    { "Hold to unmute", "SEGURE PARA ATIVAR O SOM" },
    { "Hold to power off", "SEGURE PARA DESLIGAR" },
    { "Goodbye!", "TCHAU!" },
    { "Couldn't power off", "NÃO DEU PARA DESLIGAR" },
    { "Phone setup on", "CONFIG. PELO CELULAR LIGADA" },
    { "Phone setup: %s", "CONFIG. CELULAR: %s" },
    { "Phone setup off", "CONFIG. PELO CELULAR DESLIGADA" },
    { "Resetting...", "REDEFININDO..." },
    { "RESTARTING...", "REINICIANDO..." },
};

/* The button menu (muse_menu.c), the pairing card (muse_ui.c), the latest
 * reply page and the touch settings pages: sentence case, in Montserrat
 * with its Portuguese letters (fonts/muse_font_pt_<px>.c). */
static const muse_lang_entry_t MENU[] = {
    /* Rows */
    { "Volume", "Volume" },
    { "Speaker", "Alto-falante" },
    { "Brightness", "Brilho" },
    { "Mic gain", "Ganho do mic" },
    { "Auto-sleep", "Repouso automático" },
    { "Text size", "Tamanho do texto" },
    { "Phone setup", "Config. pelo celular" },
    { "Status", "Status" },
    { "Battery", "Bateria" },
    { "Reset pairing", "Refazer pareamento" },
    { "Screen off", "Apagar a tela" },
    { "Power off", "Desligar" },
    { "Close menu", "Fechar menu" },
    /* What the talk button does */
    { "Change", "Mudar" },
    { "Toggle", "Alternar" },
    { "Open", "Abrir" },
    { "Select", "Escolher" },
    { "Close", "Fechar" },
    { "Back", "Voltar" },
    { "Reset", "Redefinir" },
    { "Cancel", "Cancelar" },
    { "Esc Back", "Esc Voltar" },
    { "Esc Cancel", "Esc Cancelar" },
    { LV_SYMBOL_DOWN " Down", LV_SYMBOL_DOWN " Descer" },
    { "Down " LV_SYMBOL_RIGHT, "Descer " LV_SYMBOL_RIGHT },
    /* Values */
    { "On", "Ligado" },
    { "Off", "Desligado" },
    { "Not set", "Não definido" },
    { "Joining", "Entrando" },
    { "Failed", "Falhou" },
    { "Not found", "Não encontrada" },
    { "Never", "Nunca" },
    { "30 s", "30 s" },
    { "1 min", "1 min" },
    { "2 min", "2 min" },
    { "5 min", "5 min" },
    { "10 min", "10 min" },
    { "Small", "Pequeno" },
    { "Medium", "Médio" },
    { "Large", "Grande" },
    { "Extra small", "Muito pequeno" },
    { "Extra large", "Muito grande" },
    { "Custom", "Personalizado" },
    /* Titles: the menu's, upper case; the others are their rows' names above */
    { "MENU", "MENU" },
    { "MENU  ^v Move  <> Change", "MENU  ^v Mover  <> Mudar" },
    /* Pages, their columns lined up as in the English */
    { "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nPhone %s\nPower %s\nVer   %s", "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nCel   %s\nEnerg %s\nVer   %s" },
    { "off", "desl." },
    { "offline", "offline" },
    { "Connected", "Conectado" },
    { "Starting", "Iniciando" },
    { "Ready to pair", "Pronto para parear" },
    { "App connected", "App conectado" },
    { "Confirm pairing", "Confirme o pareamento" },
    { "Connecting", "Conectando" },
    { "Online", "Online" },
    { "Offline", "Offline" },
    { "Error", "Erro" },
    { "Not set up", "Não configurado" },
    { "Saved", "Salvo" },
    { "Can't connect", "Sem conexão" },
    { "Unplug USB to\nmeasure how\nlong the\nbattery lasts.", "Tire o USB\npara medir\nquanto a\nbateria dura." },
    { "%s %s\nBatt  %d>%d%%\nRate  %s\nFull  %s\nOff   %s\nSleep %s\nWakes %s\nBusy  %s", "%s %s\nBat   %d>%d%%\nTaxa  %s\nCheia %s\nApag  %s\nSono  %s\nAcord %s\nOcup  %s" },
    { "On batt", "Na bateria" },
    { "Last run", "Última" },
    { "Turn Muse off?\n\nPress the %s button to turn it back on.", "Desligar o Muse?\n\nAperte o botão %s para religar." },
    { "Forget Wi-Fi and the Muse app pairing, then restart?", "Esquecer o Wi-Fi e o pareamento do app Muse e reiniciar?" },
    /* The pairing card */
    { "Pairing code", "Código de pareamento" },
    { "Enter on phone", "Digite no celular" },
    { "Enter it on your phone", "Digite no seu celular" },
    { "Press", "Aperte" },
    { "Press button", "Aperte o botão" },
    { "%s button", "Botão %s" },
    { "Press the %s button", "Aperte o botão %s" },
    { "Muse app", "App Muse" },
    { "Pair with Muse", "Parear com o app Muse" },
    { "Confirm", "Confirmar" },
    { "Tap Confirm to pair", "Toque em Confirmar" },
    /* The latest reply page (muse_latest_reply.c) */
    { "LATEST REPLY", "ÚLTIMA RESPOSTA" },
    { "No reply yet", "Ainda sem resposta" },
    { "New reply", "Nova resposta" },
    { "Latest reply unavailable", "Última resposta indisponível" },
    /* The touch settings pages (muse_settings_ui.c) */
    { "Settings", "CONFIGURAÇÕES" },
    { "Run apps", "Rodar apps" },
    { "Apps", "Apps" },
    { "Muse", "Muse" },
    { "Bluetooth", "Bluetooth" },
    { "Screen", "Tela" },
    { "Sound", "Som" },
    { "Sleep", "Repouso" },
    { "Phone camera", "Câmera do celular" },
    { "Not nearby", "Fora de alcance" },
    { "Mic only", "Só microfone" },
    { "No audio", "Sem áudio" },
    { "Muted", "Mudo" },
    { "Ready", "Pronto" },
    { "No phone", "Sem celular" },
    { "30 seconds", "30 segundos" },
    { "1 minute", "1 minuto" },
    { "2 minutes", "2 minutos" },
    { "5 minutes", "5 minutos" },
    { "Show", "Mostrar" },
    { "Hide", "Ocultar" },
    { "Password", "Senha" },
    { "Empty if it's open", "Vazio se for aberta" },
    { "Other network", "Outra rede" },
    { "Network name", "Nome da rede" },
    { "Saved networks", "Redes salvas" },
    { "Tap to forget", "Toque para esquecer" },
    { "Hidden", "Oculta" },
    { "saved", "salva" },
    { "secured", "protegida" },
    { "open", "aberta" },
    { "saved  ", "salva  " },
    { "open  ", "aberta  " },
    { "No networks found", "Nenhuma rede encontrada" },
    { "WI-FI", "WI-FI" },
    { LV_SYMBOL_REFRESH "  Scan for networks", LV_SYMBOL_REFRESH "  Procurar redes" },
    { "Other network...", "Outra rede..." },
    { "MAC address", "Endereço MAC" },
    { "Muse remembers up to 8 networks and joins the strongest one in range. Tap a saved one twice to forget it.", "O Muse lembra até 8 redes e entra na de sinal mais forte. Toque duas vezes numa rede salva para esquecê-la." },
    { "Wi-Fi is off", "Wi-Fi desligado" },
    { "No saved networks. Scan and pick one.", "Nenhuma rede salva. Procure e escolha uma." },
    { "Joining %s\n%s", "Entrando em %s\n%s" },
    { "Connected to %s\n%s", "Conectado a %s\n%s" },
    { "Connected to %s\n%s  -  %d dBm", "Conectado a %s\n%s  -  %d dBm" },
    { "No saved network nearby\nLooking again within a minute", "Nenhuma rede salva por perto\nProcurando de novo em até 1 minuto" },
    { "Couldn't join %s\n%s", "Não deu para entrar em %s\n%s" },
    { "Scanning...", "Procurando..." },
    { "PROXY", "PROXY" },
    { "Configure Proxy", "Configurar proxy" },
    { "Port", "Porta" },
    { "Username", "Usuário" },
    { "Save", "Salvar" },
    { "Proxy server", "Servidor proxy" },
    { "Host name or IP", "Nome do host ou IP" },
    { "Proxy port", "Porta do proxy" },
    { "Proxy username", "Usuário do proxy" },
    { "Proxy password", "Senha do proxy" },
    { "Invalid; select mode", "Inválido; escolha o modo" },
    { "Check server, port and authentication", "Confira servidor, porta e autenticação" },
    { "Saving...", "Salvando..." },
    { "Save failed; try again", "Não deu para salvar; tente de novo" },
    { "Busy or network forgotten; try again", "Ocupado ou rede esquecida; tente de novo" },
    { "Cloud connections use this network's proxy. Local traffic stays direct.", "As conexões com a nuvem usam o proxy desta rede. O tráfego local continua direto." },
    { "SDK token", "Token do SDK" },
    { "Empty keeps the current one; a new one restarts", "Vazio mantém o atual; um novo reinicia" },
    { "Muse server", "Servidor do Muse" },
    { "Empty for the default", "Vazio para o padrão" },
    { "VM ID", "ID da VM" },
    { "Optional", "Opcional" },
    { "Device token", "Token do aparelho" },
    { "Empty keeps the current one", "Vazio mantém o atual" },
    { "Resetting...", "Redefinindo..." },
    { "Tap again to reset", "Toque de novo para redefinir" },
    { "MUSE", "MUSE" },
    { "Server", "Servidor" },
    { "Test connection", "Testar conexão" },
    { "not paired", "não pareado" },
    { "paired", "pareado" },
    { "paired with another SDK token, pair again to use this one", "pareado com outro token do SDK; pareie de novo para usar este" },
    { "Muse app: %s\n%s", "App Muse: %s\n%s" },
    { "Set here", "Definido aqui" },
    { "Set", "Definido" },
    { "Not a token", "Não é um token" },
    { "Not allowed", "Não permitido" },
    { "Forget paired phones", "Esquecer celulares pareados" },
    { "Visible as %s", "Visível como %s" },
    { "Phone connected\n%s", "Celular conectado\n%s" },
    { "Paired", "Pareado" },
    { "Waiting for pairing", "Aguardando pareamento" },
    { "Voice notes", "Mensagens de voz" },
    { "Off: hold yellow to talk live, release to listen; blue ends the call. On: hold yellow to send a voice note. Applies to the next call.", "Desligado: segure o amarelo para falar ao vivo, solte para ouvir; o azul encerra a chamada. Ligado: segure o amarelo para mandar uma mensagem de voz. Vale a partir da próxima chamada." },
    { "Mic level", "Nível do mic" },
    { "Talk at arm's length: the bar should reach green (-30 to -15 dBFS) without going orange.", "Fale a um braço de distância: a barra deve chegar ao verde (-30 a -15 dBFS) sem passar para o laranja." },
    { LV_SYMBOL_EYE_CLOSE "  Sleep now", LV_SYMBOL_EYE_CLOSE "  Apagar agora" },
    { "Used", "Usado" },
    { "A full charge", "Carga completa" },
    { "Chip asleep", "Chip dormindo" },
    { "Wakes", "Despertares" },
    { "CPU busy", "CPU ocupada" },
    { LV_SYMBOL_REFRESH "  Start over", LV_SYMBOL_REFRESH "  Recomeçar" },
    { "Measures from unplugging USB until it's plugged back in. The gauge moves in 1% steps, so give it a few hours. Chip asleep is time in light sleep; CPU busy is time a core was running a task.", "Mede do momento em que o USB sai até ele voltar. O medidor anda de 1% sem frações, então espere algumas horas. Chip dormindo é o tempo em sono leve; CPU ocupada é o tempo em que um núcleo executou uma tarefa." },
    { "%d h %d min", "%d h %d min" },
    { "%d min", "%d min" },
    { "No battery", "Sem bateria" },
    { "Unplug USB to start measuring.", "Tire o USB para começar a medir." },
    { "On battery for %s", "Na bateria há %s" },
    { "Last run: %s on battery", "Última vez: %s na bateria" },
    { "None", "Nada" },
    { "lasts ~%d h", "dura ~%d h" },
    { "measuring", "medindo" },
    { "%d%% so far", "%d%% até agora" },
    { "Also kept awake by: %s", "Também acordado por: %s" },
    { LV_SYMBOL_POWER "  Power off", LV_SYMBOL_POWER "  Desligar" },
    { "Restart", "Reiniciar" },
};
#endif /* language tables */

static const char *lookup(const muse_lang_entry_t *t, size_t n, const char *en)
{
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(t[i].en, en)) {
            return t[i].text;
        }
    }
    return en;
}

const char *muse_lang_status(const char *en)
{
    return lookup(STATUS, sizeof(STATUS) / sizeof(STATUS[0]), en);
}

const char *muse_lang_message(const char *en)
{
    return lookup(MESSAGES, sizeof(MESSAGES) / sizeof(MESSAGES[0]), en);
}

const char *muse_lang_menu(const char *en)
{
    return lookup(MENU, sizeof(MENU) / sizeof(MENU[0]), en);
}

/* LVGL's Montserrat has ASCII and its symbols; fonts/muse_font_vi_<px>.c has
 * Montserrat's Vietnamese letters at the same size and baseline, and
 * fonts/muse_font_pt_<px>.c its Portuguese ones (with 16 px for the touch
 * settings pages). */
#if CONFIG_MUSE_UI_LANG_VI
#define LANG_FONT(px) muse_font_vi_##px
#else
#define LANG_FONT(px) muse_font_pt_##px
#define LANG_FONT_16 1
#endif
#if LV_FONT_MONTSERRAT_12
LV_FONT_DECLARE(LANG_FONT(12))
#endif
LV_FONT_DECLARE(LANG_FONT(14))
#if LANG_FONT_16 && LV_FONT_MONTSERRAT_16
LV_FONT_DECLARE(LANG_FONT(16))
#endif
LV_FONT_DECLARE(LANG_FONT(20))
LV_FONT_DECLARE(LANG_FONT(28))

const lv_font_t *muse_lang_montserrat(const lv_font_t *font)
{
    static const struct {
        const lv_font_t *base, *letters;
    } SIZES[] = {
#if LV_FONT_MONTSERRAT_12
        { &lv_font_montserrat_12, &LANG_FONT(12) },
#endif
        { &lv_font_montserrat_14, &LANG_FONT(14) },
#if LANG_FONT_16 && LV_FONT_MONTSERRAT_16
        { &lv_font_montserrat_16, &LANG_FONT(16) },
#endif
        { &lv_font_montserrat_20, &LANG_FONT(20) },
        { &lv_font_montserrat_28, &LANG_FONT(28) },
    };
    static lv_font_t copies[sizeof(SIZES) / sizeof(SIZES[0])];
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        if (font == SIZES[i].base) {
            if (!copies[i].get_glyph_dsc) {
                copies[i] = *font;
                copies[i].fallback = SIZES[i].letters;
            }
            return &copies[i];
        }
    }
    return font;
}
#else
const char *muse_lang_status(const char *en)
{
    return en;
}

const char *muse_lang_message(const char *en)
{
    return en;
}

const char *muse_lang_menu(const char *en)
{
    return en;
}

#if !CONFIG_MUSE_UI_FREEINK
const lv_font_t *muse_lang_montserrat(const lv_font_t *font)
{
    return font;
}
#endif
#endif

#if !CONFIG_MUSE_UI_FREEINK
#if CONFIG_MUSE_LATIN_FONT
LV_FONT_DECLARE(muse_font_latin_16)
#endif

#if CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define LABEL_CJK_FONT muse_font_cjk_noto_16
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define LABEL_CJK_FONT muse_font_cjk_16
#endif

const lv_font_t *muse_lang_label_font(const char *text, const lv_font_t *plain)
{
#if CONFIG_MUSE_CJK_FONT
    if (muse_text_has_cjk(text)) {
        /* The CJK font has no ASCII: a copy that falls back to unscii-16,
         * or with CONFIG_MUSE_LATIN_FONT to the accented font (ASCII
         * included), draws a mixed label's other letters too. */
        static lv_font_t cjk_fb;
        if (!cjk_fb.get_glyph_dsc) {
            cjk_fb = LABEL_CJK_FONT;
#if CONFIG_MUSE_LATIN_FONT
            cjk_fb.fallback = &muse_font_latin_16;
#else
            cjk_fb.fallback = &lv_font_unscii_16;
#endif
        }
        return &cjk_fb;
    }
#endif
#if CONFIG_MUSE_LATIN_FONT
    if (muse_text_has_latin(text)) {
        return &muse_font_latin_16;
    }
#endif
    (void)text;
    return plain;
}
#endif

#if CONFIG_MUSE_UI_LANG_PT_BR
/* The letters fonts/muse_font_pt_<px>.c and the caption font have. */
static bool pt_letter(uint32_t c)
{
    static const char LETTERS[] = "ÀÁÂÃÇÉÊÍÓÔÕÚÜàáâãçéêíóôõúü";
    const unsigned char *p = (const unsigned char *)LETTERS;
    while (*p) {
        uint32_t l = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (l == c) {
            return true;
        }
        p += 2;
    }
    return false;
}
#endif

bool muse_lang_showable(const char *text)
{
#if CONFIG_MUSE_UI_LANG_PT_BR
    bool accented = false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if (*p < 0x80) {
            p++;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80
                   && pt_letter(((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F))) {
            accented = true;
            p += 2;
        } else {
            return false;   /* anything else is folded to ASCII, as before */
        }
    }
    return accented;
#else
    (void)text;
    return false;
#endif
}
