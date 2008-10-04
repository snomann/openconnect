/*
 * Open AnyConnect (SSL + DTLS) client
 *
 * © 2008 David Woodhouse <dwmw2@infradead.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to:
 *
 *   Free Software Foundation, Inc.
 *   51 Franklin Street, Fifth Floor,
 *   Boston, MA 02110-1301 USA
 */

#include <errno.h>
#include <poll.h>
#include <limits.h>
#include <sys/select.h>
#include <signal.h>
#include <arpa/inet.h>

#include "anyconnect.h"

void queue_packet(struct pkt **q, struct pkt *new)
{
	while (*q)
		q = &(*q)->next;

	new->next = NULL;
	*q = new;
}

int queue_new_packet(struct pkt **q, int type, void *buf, int len)
{
	struct pkt *new = malloc(sizeof(struct pkt) + len);
	if (!new)
		return -ENOMEM;

	new->type = type;
	new->len = len;
	new->next = NULL;
	memcpy(new->data, buf, len);
	queue_packet(q, new);
	return 0;
}

int vpn_add_pollfd(struct anyconnect_info *vpninfo, int fd, short events)
{
	vpninfo->nfds++;
	vpninfo->pfds = realloc(vpninfo->pfds, sizeof(struct pollfd) * vpninfo->nfds);
	if (!vpninfo->pfds) {
		fprintf(stderr, "Failed to reallocate pfds\n");
		exit(1);
	}
	vpninfo->pfds[vpninfo->nfds - 1].fd = fd;
	vpninfo->pfds[vpninfo->nfds - 1].events = events;

	return vpninfo->nfds - 1;
}

static int killed;

static void handle_sigint(int sig)
{
	killed = sig;
}

int vpn_mainloop(struct anyconnect_info *vpninfo)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sigint;
	
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	while (!vpninfo->quit_reason) {
		int did_work = 0;
		int timeout = INT_MAX;

		if (vpninfo->new_dtls_ssl)
			dtls_try_handshake(vpninfo);

		if (!vpninfo->dtls_ssl && !vpninfo->new_dtls_ssl &&
		    vpninfo->new_dtls_started + vpninfo->dtls_attempt_period < time(NULL)) {
			if (verbose)
				printf("Attempt new DTLS connection\n");
			connect_dtls_socket(vpninfo);
		}
		if (vpninfo->dtls_ssl)
			did_work += dtls_mainloop(vpninfo, &timeout);

		if (vpninfo->quit_reason)
			break;

		did_work += ssl_mainloop(vpninfo, &timeout);
		if (vpninfo->quit_reason)
			break;
		
		did_work += tun_mainloop(vpninfo, &timeout);
		if (vpninfo->quit_reason)
			break;

		if (killed) {
			if (killed == SIGHUP)
				vpninfo->quit_reason = "Client received SIGHUP";
			else if (killed == SIGINT)
				vpninfo->quit_reason = "Client received SIGINT";
			else
				vpninfo->quit_reason = "Client killed";
			break;
		}

		if (did_work)
			continue;

		if (verbose)
			printf("Did no work; sleeping for %d ms...\n", timeout);

		poll(vpninfo->pfds, vpninfo->nfds, timeout);
		if (vpninfo->pfds[vpninfo->ssl_pfd].revents & POLL_HUP) {
			fprintf(stderr, "Server closed connection!\n");
			/* OpenSSL doesn't seem to cope properly with this... */
			exit(1);
		}
	}

	ssl_bye(vpninfo, vpninfo->quit_reason);
	printf("Sent quit message: %s\n", vpninfo->quit_reason);

	if (vpninfo->vpnc_script) {
		setenv("TUNDEV", vpninfo->ifname, 1);
		setenv("reason", "disconnect", 1);
		system(vpninfo->vpnc_script);
	}

	return 0;
}

/* Called when the socket is unwritable, to get the deadline for DPD.
   Returns 1 if DPD deadline has already arrived. */
int ka_stalled_dpd_time(struct keepalive_info *ka, int *timeout)
{
	time_t now, due;

	if (!ka->dpd) {
		printf("no dpd\n");
		return 0;
	}

	time(&now);
	due = ka->last_rx + (2 * ka->dpd);

	if (now > due)
		return 1;

	printf("ka_stalled in %d seconds\n", (int)(due - now));
	if (*timeout > (due - now) * 1000)
		*timeout = (due - now) * 1000;

	return 0;
}


int keepalive_action(struct keepalive_info *ka, int *timeout)
{
	time_t now = time(NULL);

	if (ka->rekey) {
		time_t due = ka->last_rekey + ka->rekey;

		if (now >= due)
			return KA_REKEY;

		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	/* DPD is bidirectional -- PKT 3 out, PKT 4 back */
	if (ka->dpd) {
		time_t due = ka->last_rx + ka->dpd;
		time_t overdue = ka->last_rx + (2 * ka->dpd);

		/* Peer didn't respond */
		if (now > overdue)
			return KA_DPD_DEAD;

		/* If we already have DPD outstanding, don't flood. Repeat by
		   all means, but only after half the DPD period. */
		if (ka->last_dpd > ka->last_rx)
			due = ka->last_dpd + ka->dpd / 2;

		/* We haven't seen a packet from this host for $DPD seconds.
		   Prod it to see if it's still alive */
		if (now >= due) {
			ka->last_dpd = now;
			return KA_DPD;
		}
		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	/* Keepalive is just client -> server */
	if (ka->keepalive) {
		time_t due = ka->last_tx + ka->keepalive;

		/* If we haven't sent anything for $KEEPALIVE seconds, send a
		   dummy packet (which the server will discard) */
		if (now >= due)
			return KA_KEEPALIVE;

		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	return KA_NONE;
}
