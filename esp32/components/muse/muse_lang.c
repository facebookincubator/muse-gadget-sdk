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
 * a translation keeps its format conversions and status names their upper
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
/* Short names, upper case like the English: they head the screen. */
static const muse_lang_entry_t STATUS[] = {
    { "WAKING UP", "ĐANG KHỞI ĐỘNG" },
    { "READY", "SẴN SÀNG" },
    { "LISTENING", "ĐANG NGHE" },
    { "THINKING", "ĐANG NGHĨ" },
    { "SPEAKING", "ĐANG NÓI" },
    { "ERROR", "LỖI" },
    { "GOODBYE", "TẠM BIỆT" },
    { "WI-FI OFF", "WI-FI ĐANG TẮT" },
    { "SET UP WI-FI", "CHƯA CÀI WI-FI" },
    { "NO WI-FI", "KHÔNG CÓ WI-FI" },
    { "RECONNECTING", "ĐANG KẾT NỐI LẠI" },
    { "CONNECTING", "ĐANG KẾT NỐI" },
    { "USB POWER", "NGUỒN USB" },
    { "CHARGING %d%%", "ĐANG SẠC %d%%" },
    { "BATTERY %d%%", "PIN %d%%" },
};

/* Sentence case: a capital with two marks (Ấ, Ễ) is cramped in 16 px. */
static const muse_lang_entry_t MESSAGES[] = {
    /* The voice path */
    { "WAKING UP...", "Đang khởi động..." },
    { "LISTENING...", "Đang nghe..." },
    { "RECORDING...", "Đang ghi âm..." },
    { "LISTENING %.1fs", "Đang nghe %.1fs" },
    { "RECORDING %.1fs", "Đang ghi âm %.1fs" },
    { "SENDING VOICE NOTE", "Đang gửi tin nhắn thoại" },
    { "NOTE SENT - WAITING FOR MUSE", "Đã gửi, đang chờ Muse" },
    { "HOLD LONGER TO TALK", "Giữ nút lâu hơn để nói" },
    { "RECORD A LONGER NOTE", "Hãy ghi âm lâu hơn" },
    { "MICROPHONE ERROR - TRY AGAIN", "Lỗi mic - thử lại" },
    { "SET UP MUSE FIRST", "Hãy cài đặt Muse trước" },
    { "NO WI-FI", "Không có Wi-Fi" },
    { "AUDIO INIT FAILED", "Lỗi khởi động âm thanh" },
    /* Notes saved to send later */
    { "COULDN'T SAVE THE NOTE", "Không lưu được tin nhắn" },
    { "SAVED, WILL TRY AGAIN", "Đã lưu, sẽ thử lại" },
    { "SAVED, SENDS WHEN ONLINE", "Đã lưu, sẽ gửi khi có mạng" },
    { "SENDING SAVED NOTE", "Đang gửi tin nhắn đã lưu" },
    { "SAVED NOTE SENT", "Đã gửi tin nhắn đã lưu" },
    { "COULDN'T SEND A SAVED NOTE", "Không gửi được tin nhắn đã lưu" },
    { "SAVED NOTE: WILL TRY AGAIN", "Tin nhắn đã lưu: sẽ thử lại" },
    { "NOTES STILL WAITING TO SEND", "Còn tin nhắn chờ gửi" },
    { "MAY HAVE SENT - SEE MUSE APP", "Có thể đã gửi: xem app Muse" },
    /* A turn that failed */
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
    { "SPEAKER ON", "Đã bật loa" },
    { "SPEAKER OFF", "Đã tắt loa" },
    { "HOLD TO MUTE", "Giữ để tắt tiếng" },
    { "HOLD TO UNMUTE", "Giữ để bật tiếng" },
    { "HOLD TO POWER OFF", "Giữ để tắt máy" },
    { "GOODBYE!", "Tạm biệt!" },
    { "COULDN'T POWER OFF", "Không tắt được máy" },
    { "PHONE SETUP ON", "Đã bật cài đặt qua điện thoại" },
    { "PHONE SETUP: %s", "Cài qua ĐT: %s" },
    { "PHONE SETUP OFF", "Đã tắt cài đặt qua điện thoại" },
    { "RESETTING...", "Đang đặt lại..." },
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
    /* Titles, upper case */
    { "MENU", "CÀI ĐẶT" },
    { "MENU  ^v Move  <> Change", "CÀI ĐẶT  ^v Chọn  <> Đổi" },
    { "STATUS", "TRẠNG THÁI" },
    { "BATTERY", "PIN" },
    { "POWER OFF", "TẮT MÁY" },
    { "RESET PAIRING", "ĐẶT LẠI GHÉP NỐI" },
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
    { "Tap screen", "Chạm màn hình" },
    { "Tap to confirm pairing", "Chạm để xác nhận ghép nối" },
    { "Press", "Bấm" },
    { "Press button", "Bấm nút" },
    { "%s button", "Nút %s" },
    { "Press the %s button", "Bấm nút %s" },
    { "Muse app", "App Muse" },
    { "Pair with Muse app", "Ghép nối với app Muse" },
};

#elif CONFIG_MUSE_UI_LANG_PT_BR
/* Short names, upper case like the English: they head the screen. No
 * accents, so the unscii status font draws them as it is. */
static const muse_lang_entry_t STATUS[] = {
    { "WAKING UP", "INICIANDO" },
    { "READY", "PRONTO" },
    { "LISTENING", "OUVINDO" },
    { "THINKING", "PENSANDO" },
    { "SPEAKING", "FALANDO" },
    { "ERROR", "ERRO" },
    { "GOODBYE", "TCHAU" },
    { "WI-FI OFF", "WI-FI DESLIGADO" },
    { "SET UP WI-FI", "CONFIGURE O WI-FI" },
    { "NO WI-FI", "SEM WI-FI" },
    { "RECONNECTING", "RECONECTANDO" },
    { "CONNECTING", "CONECTANDO" },
    { "USB POWER", "ENERGIA USB" },
    { "CHARGING %d%%", "CARREGANDO %d%%" },
    { "BATTERY %d%%", "BATERIA %d%%" },
};

/* Upper case like the English; accented capitals come from the caption
 * font (CONFIG_MUSE_LATIN_FONT). */
static const muse_lang_entry_t MESSAGES[] = {
    /* The voice path */
    { "WAKING UP...", "INICIANDO..." },
    { "LISTENING...", "OUVINDO..." },
    { "RECORDING...", "GRAVANDO..." },
    { "LISTENING %.1fs", "OUVINDO %.1fs" },
    { "RECORDING %.1fs", "GRAVANDO %.1fs" },
    { "SENDING VOICE NOTE", "ENVIANDO ÁUDIO" },
    { "NOTE SENT - WAITING FOR MUSE", "ENVIADO - AGUARDANDO O MUSE" },
    { "HOLD LONGER TO TALK", "SEGURE MAIS PARA FALAR" },
    { "RECORD A LONGER NOTE", "GRAVE UM ÁUDIO MAIS LONGO" },
    { "MICROPHONE ERROR - TRY AGAIN", "ERRO NO MICROFONE - TENTE DE NOVO" },
    { "SET UP MUSE FIRST", "CONFIGURE O MUSE PRIMEIRO" },
    { "NO WI-FI", "SEM WI-FI" },
    { "AUDIO INIT FAILED", "FALHA AO INICIAR O ÁUDIO" },
    { "MUSE HAS AN ANSWER", "O MUSE TEM UMA RESPOSTA" },
    { "NO REPLY YET", "AINDA SEM RESPOSTA" },
    { "ONE MOMENT...", "UM MOMENTO..." },
    { "INPUT LOST - PRESS AGAIN", "ENTRADA PERDIDA - APERTE DE NOVO" },
    { "MIC BUFFER FULL - RELEASE AND RETRY", "MIC CHEIO - SOLTE E TENTE DE NOVO" },
    { "REPLY MISSED - HOLD TO TRY AGAIN", "RESPOSTA PERDIDA - SEGURE E REPITA" },
    /* Notes saved to send later */
    { "COULDN'T SAVE THE NOTE", "NÃO DEU PARA SALVAR O ÁUDIO" },
    { "SAVED, WILL TRY AGAIN", "SALVO, VOU TENTAR DE NOVO" },
    { "SAVED, SENDS WHEN ONLINE", "SALVO, ENVIA QUANDO TIVER REDE" },
    { "SENDING SAVED NOTE", "ENVIANDO ÁUDIO SALVO" },
    { "SAVED NOTE SENT", "ÁUDIO SALVO ENVIADO" },
    { "COULDN'T SEND A SAVED NOTE", "NÃO DEU PARA ENVIAR O ÁUDIO SALVO" },
    { "SAVED NOTE: WILL TRY AGAIN", "ÁUDIO SALVO: VOU TENTAR DE NOVO" },
    { "NOTES STILL WAITING TO SEND", "ÁUDIOS AINDA AGUARDANDO ENVIO" },
    { "MAY HAVE SENT - SEE MUSE APP", "PODE TER SIDO ENVIADO: VEJA NO APP" },
    /* A turn that failed */
    { "CAN'T REACH MUSE", "SEM CONEXÃO COM O MUSE" },
    { "MUSE NOT SET UP", "MUSE NÃO CONFIGURADO" },
    { "LOST CONNECTION TO MUSE", "CONEXÃO COM O MUSE PERDIDA" },
    { "NO REPLY FROM MUSE", "O MUSE NÃO RESPONDEU" },
    { "DIDN'T CATCH THAT", "NÃO ENTENDI, FALE DE NOVO" },
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
    { "CONNECTING TO MUSE...", "CONECTANDO AO MUSE..." },
    { "HANGING UP...", "DESLIGANDO..." },
    { "HOLD YELLOW TO TALK", "SEGURE O AMARELO PARA FALAR" },
    { "IN A CALL - PRESS TO HANG UP", "EM CHAMADA - APERTE P/ DESLIGAR" },
    /* Buttons and power */
    { "SPEAKER ON", "ALTO-FALANTE LIGADO" },
    { "SPEAKER OFF", "ALTO-FALANTE DESLIGADO" },
    { "HOLD TO MUTE", "SEGURE PARA SILENCIAR" },
    { "HOLD TO UNMUTE", "SEGURE PARA ATIVAR O SOM" },
    { "HOLD TO POWER OFF", "SEGURE PARA DESLIGAR" },
    { "GOODBYE!", "TCHAU!" },
    { "COULDN'T POWER OFF", "NÃO DEU PARA DESLIGAR" },
    { "PHONE SETUP ON", "CONFIG. PELO CELULAR LIGADA" },
    { "PHONE SETUP: %s", "CONFIG. CELULAR: %s" },
    { "PHONE SETUP OFF", "CONFIG. PELO CELULAR DESLIGADA" },
    { "RESETTING...", "REDEFININDO..." },
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
    /* Titles, upper case */
    { "MENU", "MENU" },
    { "MENU  ^v Move  <> Change", "MENU  ^v Mover  <> Mudar" },
    { "STATUS", "STATUS" },
    { "BATTERY", "BATERIA" },
    { "POWER OFF", "DESLIGAR" },
    { "RESET PAIRING", "REFAZER PAREAMENTO" },
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
    { "Tap screen", "Toque na tela" },
    { "Tap to confirm pairing", "Toque para confirmar" },
    { "Press", "Aperte" },
    { "Press button", "Aperte o botão" },
    { "%s button", "Botão %s" },
    { "Press the %s button", "Aperte o botão %s" },
    { "Muse app", "App Muse" },
    { "Pair with Muse app", "Parear com o app Muse" },
    /* The latest reply page (muse_latest_reply.c) */
    { "LATEST REPLY", "ÚLTIMA RESPOSTA" },
    { "No reply yet", "Ainda sem resposta" },
    { "New reply", "Nova resposta" },
    { "Latest reply unavailable", "Última resposta indisponível" },
    /* The touch settings pages (muse_settings_ui.c) */
    { "SETTINGS", "CONFIGURAÇÕES" },
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
    { "Vol %d%%", "Vol %d%%" },
    { "Muted", "Mudo" },
    { "Ready", "Pronto" },
    { "No phone", "Sem celular" },
    { "30 seconds", "30 segundos" },
    { "1 minute", "1 minuto" },
    { "2 minutes", "2 minutos" },
    { "5 minutes", "5 minutos" },
    { "10 minutes", "10 minutos" },
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
    { "Pair with the Muse app to use your account; a device token here overrides it, and a long one is easier to send over Bluetooth. The VM ID picks one of your VMs. The SDK token is from gadgets.muse.ai; one set here replaces the firmware's. Reset pairing forgets Wi-Fi and the app pairing, then restarts.", "Pareie com o app Muse para usar sua conta; um token do aparelho aqui tem prioridade, e um longo é mais fácil de enviar por Bluetooth. O ID da VM escolhe uma das suas VMs. O token do SDK vem de gadgets.muse.ai; um definido aqui substitui o do firmware. Refazer pareamento esquece o Wi-Fi e o pareamento do app e reinicia." },
    { "not paired", "não pareado" },
    { "paired", "pareado" },
    { "paired with another SDK token, pair again to use this one", "pareado com outro token do SDK; pareie de novo para usar este" },
    { "Muse app: %s\n%s", "App Muse: %s\n%s" },
    { "Set here", "Definido aqui" },
    { "Set", "Definido" },
    { "Not a token", "Não é um token" },
    { "BLUETOOTH", "BLUETOOTH" },
    { "Forget paired phones", "Esquecer celulares pareados" },
    { "When on, Muse is visible to phones nearby. Open tools/muse/ble_setup.html in Chrome, connect, and enter the code Muse shows to pair.", "Ligado, o Muse fica visível para celulares próximos. Abra tools/muse/ble_setup.html no Chrome, conecte e digite o código que o Muse mostrar para parear." },
    { "Visible as %s", "Visível como %s" },
    { "Phone connected\n%s", "Celular conectado\n%s" },
    { "Paired", "Pareado" },
    { "Waiting for pairing", "Aguardando pareamento" },
    { "SCREEN", "TELA" },
    { "SOUND", "SOM" },
    { "Voice notes", "Mensagens de voz" },
    { "Off: hold yellow to talk live, release to listen; blue ends the call. On: hold yellow to send a voice note. Applies to the next call.", "Desligado: segure o amarelo para falar ao vivo, solte para ouvir; o azul encerra a chamada. Ligado: segure o amarelo para mandar uma mensagem de voz. Vale a partir da próxima chamada." },
    { "Mic level", "Nível do mic" },
    { "Talk at arm's length: the bar should reach green (-30 to -15 dBFS) without going orange.", "Fale a um braço de distância: a barra deve chegar ao verde (-30 a -15 dBFS) sem passar para o laranja." },
    { "AUTO-SLEEP", "REPOUSO" },
    { "Turn the screen off after Muse has been idle for:", "Apagar a tela depois que o Muse ficar parado por:" },
    { LV_SYMBOL_EYE_CLOSE "  Sleep now", LV_SYMBOL_EYE_CLOSE "  Apagar agora" },
    { "Tap the screen or press either button to wake.", "Toque na tela ou aperte um botão para acordar." },
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
    { "POWER", "ENERGIA" },
    { "Power Muse off completely?", "Desligar o Muse por completo?" },
    { LV_SYMBOL_POWER "  Power off", LV_SYMBOL_POWER "  Desligar" },
    { LV_SYMBOL_REFRESH "  Restart", LV_SYMBOL_REFRESH "  Reiniciar" },
    { "Tap the screen to turn it back on.", "Toque na tela para religar." },
    { "Press the %s button to turn it back on. To just turn the screen off, press the %s button.", "Aperte o botão %s para religar. Para só apagar a tela, aperte o botão %s." },
    { "Press the %s button to turn it back on.", "Aperte o botão %s para religar." },
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
