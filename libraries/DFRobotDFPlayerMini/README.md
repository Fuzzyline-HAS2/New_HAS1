# Local bounded-I/O patch

Vendored from DFRobotDFPlayerMini 1.0.6. Original source and licensing notice
are preserved below. Local changes:

- Bound the complete previous-command ACK wait using setTimeOut(), even if an
  unread finish/error/timeout event is pending; timeout publishes TimeOut and
  allows the next command to be sent, as the original no-event timeout did.
- Process at most 10 UART bytes per available() call; partial parser state is
  retained. This lets caller deadlines run even with continuous noise.
- Clear a retained PlayFinished before a new play/playFolder/playMp3Folder/
  playLargeFolder command is transmitted. It belongs to an older playback,
  including when it arrived during the previous-command ACK wait.
- Before those playback commands, also drain UART bytes and partial frames with
  a separate setTimeOut() deadline. On a sustained flood or truncated frame,
  report TimeOut, reset parser state, and skip that playback so a subsequent
  healthy command can recover. A send can therefore spend up to two timeout
  periods (previous ACK plus pre-play drain), rather than waiting indefinitely.
- Expose hasPendingInput() separately from event availability. The generator's
  cooperative battery preparation checks it before starting audio and resets
  partial parser state after its own two-second preparation deadline.

The protocol has no command sequence IDs: a delayed ACK/finish arriving after
transmission cannot be reliably attributed to a playback by this library.
Serial write() still relies on the supplied Stream implementation returning.

---

# DFPlayer - A Mini MP3 Player For Arduino


DFPlayer - A Mini MP3 Player For Arduino
https://www.dfrobot.com/index.php?route=product/product&product_id=1121

This example shows the all the function of library for DFPlayer.

Created 2016-12-07
By [Angelo qiao](Angelo.qiao@dfrobot.com)

GNU Lesser General Public License.
See <http://www.gnu.org/licenses/> for details.
All above must be included in any redistribution

Notice and Trouble shooting

1.Connection and Diagram can be found here
https://www.dfrobot.com/wiki/index.php/DFPlayer_Mini_SKU:DFR0299#Connection_Diagram
2.This code is tested on Arduino Uno, Leonardo, Mega boards.
