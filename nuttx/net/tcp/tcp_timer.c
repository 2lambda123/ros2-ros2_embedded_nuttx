/****************************************************************************
 * net/tcp/tcp_timer.c
 * Poll for the availability of TCP TX data
 *
 *   Copyright (C) 2007-2010 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Adapted for NuttX from logic in uIP which also has a BSD-like license:
 *
 *   Original author Adam Dunkels <adam@dunkels.com>
 *   Copyright () 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_NET) && defined(CONFIG_NET_TCP)

#include <stdint.h>
#include <debug.h>

#include <nuttx/net/netconfig.h>
#include <nuttx/net/uip.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/tcp.h>
#include <nuttx/net/netstats.h>

#include "devif/devif.h"
#include "tcp/tcp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tcp_timer
 *
 * Description:
 *   Handle a TCP timer expiration for the provided TCP connection
 *
 * Parameters:
 *   dev  - The device driver structure to use in the send operation
 *   conn - The TCP "connection" to poll for TX data
 *   hsed - The polling interval in halves of a second
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

void tcp_timer(FAR struct net_driver_s *dev, FAR struct tcp_conn_s *conn,
               int hsec)
{
  uint8_t result;

  dev->d_snddata = &dev->d_buf[UIP_IPTCPH_LEN + UIP_LLH_LEN];
  dev->d_appdata = &dev->d_buf[UIP_IPTCPH_LEN + UIP_LLH_LEN];

  /* Increase the TCP sequence number */

  tcp_nextsequence();

  /* Reset the length variables. */

  dev->d_len    = 0;
  dev->d_sndlen = 0;

  /* Check if the connection is in a state in which we simply wait
   * for the connection to time out. If so, we increase the
   * connection's timer and remove the connection if it times
   * out.
   */

  if (conn->tcpstateflags == UIP_TIME_WAIT ||
      conn->tcpstateflags == UIP_FIN_WAIT_2)
    {
      /* Increment the connection timer */

      conn->timer += hsec;
      if (conn->timer >= UIP_TIME_WAIT_TIMEOUT)
        {
          conn->tcpstateflags = UIP_CLOSED;

          /* Notify upper layers about the timeout */

          result = tcp_callback(dev, conn, UIP_TIMEDOUT);

          nllvdbg("TCP state: UIP_CLOSED\n");
        }
    }
  else if (conn->tcpstateflags != UIP_CLOSED)
    {
      /* If the connection has outstanding data, we increase the connection's
       * timer and see if it has reached the RTO value in which case we
       * retransmit.
       */

      if (conn->unacked > 0)
        {
          /* The connection has outstanding data */

          if (conn->timer > hsec)
            {
              /* Will not yet decrement to zero */

              conn->timer -= hsec;
            }
          else
            {
              /* Will decrement to zero */

              conn->timer = 0;

              /* Should we close the connection? */

              if (
#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
                  conn->expired > 0 ||
#else
                  conn->nrtx == UIP_MAXRTX ||
#endif
                  ((conn->tcpstateflags == UIP_SYN_SENT ||
                    conn->tcpstateflags == UIP_SYN_RCVD) &&
                    conn->nrtx == UIP_MAXSYNRTX)
                 )
                {
                  conn->tcpstateflags = UIP_CLOSED;
                  nllvdbg("TCP state: UIP_CLOSED\n");

                  /* We call tcp_callback() with UIP_TIMEDOUT to
                   * inform the application that the connection has
                   * timed out.
                   */

                  result = tcp_callback(dev, conn, UIP_TIMEDOUT);

                  /* We also send a reset packet to the remote host. */

                  tcp_send(dev, conn, TCP_RST | TCP_ACK, UIP_IPTCPH_LEN);
                  goto done;
                }

             /* Exponential backoff. */

              conn->timer = UIP_RTO << (conn->nrtx > 4 ? 4: conn->nrtx);
              (conn->nrtx)++;

              /* Ok, so we need to retransmit. We do this differently
               * depending on which state we are in. In ESTABLISHED, we
               * call upon the application so that it may prepare the
               * data for the retransmit. In SYN_RCVD, we resend the
               * SYNACK that we sent earlier and in LAST_ACK we have to
               * retransmit our FINACK.
               */

#ifdef CONFIG_NET_STATISTICS
              g_netstats.tcp.rexmit++;
#endif
              switch(conn->tcpstateflags & UIP_TS_MASK)
                {
                  case UIP_SYN_RCVD:
                    /* In the SYN_RCVD state, we should retransmit our
                     * SYNACK.
                     */

                    tcp_ack(dev, conn, TCP_ACK | TCP_SYN);
                    goto done;

                  case UIP_SYN_SENT:
                    /* In the SYN_SENT state, we retransmit out SYN. */

                    tcp_ack(dev, conn, TCP_SYN);
                    goto done;

                  case UIP_ESTABLISHED:
                    /* In the ESTABLISHED state, we call upon the application
                     * to do the actual retransmit after which we jump into
                     * the code for sending out the packet.
                     */

                    result = tcp_callback(dev, conn, UIP_REXMIT);
                    tcp_rexmit(dev, conn, result);
                    goto done;

                  case UIP_FIN_WAIT_1:
                  case UIP_CLOSING:
                  case UIP_LAST_ACK:
                    /* In all these states we should retransmit a FINACK. */

                    tcp_send(dev, conn, TCP_FIN | TCP_ACK, UIP_IPTCPH_LEN);
                    goto done;
                }
            }
        }

      /* The connection does not have outstanding data */

      else if ((conn->tcpstateflags & UIP_TS_MASK) == UIP_ESTABLISHED)
        {
          /* If there was no need for a retransmission, we poll the
           * application for new data.
           */

          result = tcp_callback(dev, conn, UIP_POLL);
          tcp_appsend(dev, conn, result);
          goto done;
        }
    }

  /* Nothing to be done */

  dev->d_len = 0;

done:
  return;
}

#endif /* CONFIG_NET && CONFIG_NET_TCP */