/* dnsmasq is Copyright (c) 2000-2025 Simon Kelley

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 dated June, 1991, or
   (at your option) version 3 dated 29 June, 2007.
 
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
     
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dnsmasq.h"
#include "dnsmasq_interface.h"
#include "log.h"

#ifdef HAVE_LINUX_NETWORK

int indextoname(int fd, int index, char *name)
{
  struct ifreq ifr;
  
  if (index == 0)
    return 0;

  ifr.ifr_ifindex = index;
  if (ioctl(fd, SIOCGIFNAME, &ifr) == -1)
    return 0;

  safe_strncpy(name, ifr.ifr_name, IF_NAMESIZE);

 return 1;
}


#elif defined(HAVE_SOLARIS_NETWORK)

#include <zone.h>
#include <alloca.h>
#ifndef LIFC_UNDER_IPMP
#  define LIFC_UNDER_IPMP 0
#endif

int indextoname(int fd, int index, char *name)
{
  int64_t lifc_flags;
  struct lifnum lifn;
  int numifs, bufsize, i;
  struct lifconf lifc;
  struct lifreq *lifrp;
  
  if (index == 0)
    return 0;
  
  if (getzoneid() == GLOBAL_ZONEID) 
    {
      if (!if_indextoname(index, name))
	return 0;
      return 1;
    }
  
  lifc_flags = LIFC_NOXMIT | LIFC_TEMPORARY | LIFC_ALLZONES | LIFC_UNDER_IPMP;
  lifn.lifn_family = AF_UNSPEC;
  lifn.lifn_flags = lifc_flags;
  if (ioctl(fd, SIOCGLIFNUM, &lifn) < 0) 
    return 0;
  
  numifs = lifn.lifn_count;
  bufsize = numifs * sizeof(struct lifreq);
  
  lifc.lifc_family = AF_UNSPEC;
  lifc.lifc_flags = lifc_flags;
  lifc.lifc_len = bufsize;
  lifc.lifc_buf = alloca(bufsize);
  
  if (ioctl(fd, SIOCGLIFCONF, &lifc) < 0)  
    return 0;
  
  lifrp = lifc.lifc_req;
  for (i = lifc.lifc_len / sizeof(struct lifreq); i; i--, lifrp++) 
    {
      struct lifreq lifr;
      safe_strncpy(lifr.lifr_name, lifrp->lifr_name, IF_NAMESIZE);
      if (ioctl(fd, SIOCGLIFINDEX, &lifr) < 0) 
	return 0;
      
      if (lifr.lifr_index == index) {
	safe_strncpy(name, lifr.lifr_name, IF_NAMESIZE);
	return 1;
      }
    }
  return 0;
}


#else

int indextoname(int fd, int index, char *name)
{ 
  (void)fd;

  if (index == 0 || !if_indextoname(index, name))
    return 0;

  return 1;
}

#endif

int iface_check(int family, union all_addr *addr, char *name, int *auth)
{
  struct iname *tmp;
  int ret = 1, match_addr = 0;

  /* Note: have to check all and not bail out early, so that we set the "used" flags.
     May be called with family == AF_LOCAL to check interface by name only. */
  
  if (daemon->if_names || daemon->if_addrs)
    {
      ret = 0;

      for (tmp = daemon->if_names; tmp; tmp = tmp->next)
	if (tmp->name && wildcard_match(tmp->name, name))
	  {
	    tmp->flags |= INAME_USED;
	    ret = 1;
	  }
	        
      if (addr)
	for (tmp = daemon->if_addrs; tmp; tmp = tmp->next)
	  if (tmp->addr.sa.sa_family == family)
	    {
	      if (family == AF_INET &&
		  tmp->addr.in.sin_addr.s_addr == addr->addr4.s_addr)
		{
		  tmp->flags |= INAME_USED;
		  ret = match_addr = 1;
		}
	      else if (family == AF_INET6 &&
		       IN6_ARE_ADDR_EQUAL(&tmp->addr.in6.sin6_addr, 
					  &addr->addr6))
		{
		  tmp->flags |= INAME_USED;
		  ret = match_addr = 1;
		}
	    }          
    }
  
  if (!match_addr)
    for (tmp = daemon->if_except; tmp; tmp = tmp->next)
      if (tmp->name && wildcard_match(tmp->name, name))
	ret = 0;
    
  if (auth)
    {
      *auth = 0;

      for (tmp = daemon->authinterface; tmp; tmp = tmp->next)
	if (tmp->name)
	  {
	    if (strcmp(tmp->name, name) == 0 &&
		(tmp->addr.sa.sa_family == 0 || tmp->addr.sa.sa_family == family))
	      break;
	  }
	else if (addr && tmp->addr.sa.sa_family == AF_INET && family == AF_INET &&
		 tmp->addr.in.sin_addr.s_addr == addr->addr4.s_addr)
	  break;
	else if (addr && tmp->addr.sa.sa_family == AF_INET6 && family == AF_INET6 &&
		 IN6_ARE_ADDR_EQUAL(&tmp->addr.in6.sin6_addr, &addr->addr6))
	  break;
      
      if (tmp) 
	{
	  *auth = 1;
	  ret = 1;
	}
    }

  return ret; 
}


/* Fix for problem that the kernel sometimes reports the loopback interface as the
   arrival interface when a packet originates locally, even when sent to address of 
   an interface other than the loopback. Accept packet if it arrived via a loopback 
   interface, even when we're not accepting packets that way, as long as the destination
   address is one we're believing. Interface list must be up-to-date before calling. */
int loopback_exception(int fd, int family, union all_addr *addr, char *name)    
{
  struct ifreq ifr;
  struct irec *iface;

  safe_strncpy(ifr.ifr_name, name, IF_NAMESIZE);
  if (ioctl(fd, SIOCGIFFLAGS, &ifr) != -1 &&
      ifr.ifr_flags & IFF_LOOPBACK)
    {
      for (iface = daemon->interfaces; iface; iface = iface->next)
	if (iface->addr.sa.sa_family == family)
	  {
	    if (family == AF_INET)
	      {
		if (iface->addr.in.sin_addr.s_addr == addr->addr4.s_addr)
		  return 1;
	      }
	    else if (IN6_ARE_ADDR_EQUAL(&iface->addr.in6.sin6_addr, &addr->addr6))
	      return 1;
	  }
    }
  return 0;
}

/* If we're configured with something like --interface=eth0:0 then we'll listen correctly
   on the relevant address, but the name of the arrival interface, derived from the
   index won't match the config. Check that we found an interface address for the arrival 
   interface: daemon->interfaces must be up-to-date. */
int label_exception(int index, int family, union all_addr *addr)
{
  struct irec *iface;

  /* labels only supported on IPv4 addresses. */
  if (family != AF_INET)
    return 0;

  for (iface = daemon->interfaces; iface; iface = iface->next)
    if (iface->index == index && iface->addr.sa.sa_family == AF_INET &&
	iface->addr.in.sin_addr.s_addr == addr->addr4.s_addr)
      return 1;

  return 0;
}

struct iface_param {
  struct addrlist *spare;
  int fd;
};

static int iface_allowed(struct iface_param *param, int if_index, char *label,
			 union mysockaddr *addr, struct in_addr netmask, int prefixlen, int iface_flags) 
{
  struct irec *iface;
  struct cond_domain *cond;
  int loopback;
  struct ifreq ifr;
  int tftp_ok = !!option_bool(OPT_TFTP);
  int dhcp4_ok = 1;
  int dhcp6_ok = 1;
  int auth_dns = 0;
  int is_label = 0;
#if defined(HAVE_DHCP) || defined(HAVE_TFTP)
  struct iname *tmp;
#endif

  (void)prefixlen;

  if (!indextoname(param->fd, if_index, ifr.ifr_name) ||
      ioctl(param->fd, SIOCGIFFLAGS, &ifr) == -1)
    return 0;
   
  loopback = ifr.ifr_flags & IFF_LOOPBACK;
  
  if (loopback)
    dhcp4_ok = dhcp6_ok = 0;
  
  if (!label)
    label = ifr.ifr_name;
  else
    is_label = strcmp(label, ifr.ifr_name);
 
  /* maintain a list of all addresses on all interfaces for --local-service option */
  if (option_bool(OPT_LOCAL_SERVICE))
    {
      struct addrlist *al;

      if (param->spare)
	{
	  al = param->spare;
	  param->spare = al->next;
	}
      else
	al = whine_malloc(sizeof(struct addrlist));
      
      if (al)
	{
	  al->next = daemon->interface_addrs;
	  daemon->interface_addrs = al;
	  al->prefixlen = prefixlen;
	  
	  if (addr->sa.sa_family == AF_INET)
	    {
	      al->addr.addr4 = addr->in.sin_addr;
	      al->flags = 0;
	    }
	  else
	    {
	      al->addr.addr6 = addr->in6.sin6_addr;
	      al->flags = ADDRLIST_IPV6;
	    } 
	}
    }
  
  if (addr->sa.sa_family != AF_INET6 || !IN6_IS_ADDR_LINKLOCAL(&addr->in6.sin6_addr))
    {
      struct interface_name *int_name;
      struct addrlist *al;
#ifdef HAVE_AUTH
      struct auth_zone *zone;
      struct auth_name_list *name;

      /* Find subnets in auth_zones */
      for (zone = daemon->auth_zones; zone; zone = zone->next)
	for (name = zone->interface_names; name; name = name->next)
	  if (wildcard_match(name->name, label))
	    {
	      if (addr->sa.sa_family == AF_INET && (name->flags & AUTH4))
		{
		  if (param->spare)
		    {
		      al = param->spare;
		      param->spare = al->next;
		    }
		  else
		    al = whine_malloc(sizeof(struct addrlist));
		  
		  if (al)
		    {
		      al->next = zone->subnet;
		      zone->subnet = al;
		      al->prefixlen = prefixlen;
		      al->addr.addr4 = addr->in.sin_addr;
		      al->flags = 0;
		    }
		}
	      
	      if (addr->sa.sa_family == AF_INET6 && (name->flags & AUTH6))
		{
		  if (param->spare)
		    {
		      al = param->spare;
		      param->spare = al->next;
		    }
		  else
		    al = whine_malloc(sizeof(struct addrlist));
		  
		  if (al)
		    {
		      al->next = zone->subnet;
		      zone->subnet = al;
		      al->prefixlen = prefixlen;
		      al->addr.addr6 = addr->in6.sin6_addr;
		      al->flags = ADDRLIST_IPV6;
		    }
		} 
	    }
#endif
       
      /* Update addresses from interface_names. These are a set independent
	 of the set we're listening on. */  
      for (int_name = daemon->int_names; int_name; int_name = int_name->next)
	if (strncmp(label, int_name->intr, IF_NAMESIZE) == 0)
	  {
	    struct addrlist *lp;

	    al = NULL;
	    
	    if (addr->sa.sa_family == AF_INET && (int_name->flags & (IN4 | INP4)))
	      {
		struct in_addr newaddr = addr->in.sin_addr;
		
		if (int_name->flags & INP4)
		  newaddr.s_addr = (addr->in.sin_addr.s_addr & netmask.s_addr) |
		    (int_name->proto4.s_addr & ~netmask.s_addr);
		
		/* check for duplicates. */
		for (lp = int_name->addr; lp; lp = lp->next)
		  if (lp->flags == 0 && lp->addr.addr4.s_addr == newaddr.s_addr)
		    break;
		
		if (!lp)
		  {
		    if (param->spare)
		      {
			al = param->spare;
			param->spare = al->next;
		      }
		    else
		      al = whine_malloc(sizeof(struct addrlist));

		    if (al)
		      {
			al->flags = 0;
			al->addr.addr4 = newaddr;
		      }
		  }
	      }

	    if (addr->sa.sa_family == AF_INET6 && (int_name->flags & (IN6 | INP6)))
	      {
		struct in6_addr newaddr = addr->in6.sin6_addr;
		
		if (int_name->flags & INP6)
		  {
		    int i;

		    for (i = 0; i < 16; i++)
		      {
			int bits = ((i+1)*8) - prefixlen;
		       
			if (bits >= 8)
			  newaddr.s6_addr[i] = int_name->proto6.s6_addr[i];
			else if (bits >= 0)
			  {
			    unsigned char mask = 0xff << bits;
			    newaddr.s6_addr[i] =
			      (addr->in6.sin6_addr.s6_addr[i] & mask) |
			      (int_name->proto6.s6_addr[i] & ~mask);
			  }
		      }
		  }
		
		/* check for duplicates. */
		for (lp = int_name->addr; lp; lp = lp->next)
		  if ((lp->flags & ADDRLIST_IPV6) &&
		      IN6_ARE_ADDR_EQUAL(&lp->addr.addr6, &newaddr))
		    break;
					
		if (!lp)
		  {
		    if (param->spare)
		      {
			al = param->spare;
			param->spare = al->next;
		      }
		    else
		      al = whine_malloc(sizeof(struct addrlist));
		    
		    if (al)
		      {
			al->flags = ADDRLIST_IPV6;
			al->addr.addr6 = newaddr;

			/* Privacy addresses and addresses still undergoing DAD and deprecated addresses
			   don't appear in forward queries, but will in reverse ones. */
			if (!(iface_flags & IFACE_PERMANENT) || (iface_flags & (IFACE_DEPRECATED | IFACE_TENTATIVE)))
			  al->flags |= ADDRLIST_REVONLY;
		      }
		  }
	      }
	    
	    if (al)
	      {
		al->next = int_name->addr;
		int_name->addr = al;
	      }
	  }
    }

  /* Update addresses for domain=<domain>,<interface> */
  for (cond = daemon->cond_domain; cond; cond = cond->next)
    if (cond->interface && strncmp(label, cond->interface, IF_NAMESIZE) == 0)
      {
	struct addrlist *al;

	if (param->spare)
	  {
	    al = param->spare;
	    param->spare = al->next;
	  }
	else
	  al = whine_malloc(sizeof(struct addrlist));

	if (addr->sa.sa_family == AF_INET)
	  {
	    al->addr.addr4 = addr->in.sin_addr;
	    al->flags = 0;
	  }
	else
	  {
	    al->addr.addr6 =  addr->in6.sin6_addr;
	    al->flags = ADDRLIST_IPV6;
	  }

	al->prefixlen = prefixlen;
	al->next = cond->al;
	cond->al = al;
      }
  
  /* check whether the interface IP has been added already 
     we call this routine multiple times. */
  for (iface = daemon->interfaces; iface; iface = iface->next) 
    if (sockaddr_isequal(&iface->addr, addr) && iface->index == if_index)
      {
	iface->dad = !!(iface_flags & IFACE_TENTATIVE);
	iface->found = 1; /* for garbage collection */
	iface->netmask = netmask;
	return 1;
      }

 /* If we are restricting the set of interfaces to use, make
     sure that loopback interfaces are in that set. */
  if (daemon->if_names && loopback)
    {
      struct iname *lo;
      for (lo = daemon->if_names; lo; lo = lo->next)
	if (lo->name && strcmp(lo->name, ifr.ifr_name) == 0)
	  break;
      
      if (!lo && (lo = whine_malloc(sizeof(struct iname)))) 
	{
	  if ((lo->name = whine_malloc(strlen(ifr.ifr_name)+1)))
	    {
	      strcpy(lo->name, ifr.ifr_name);
	      lo->flags |= INAME_USED;
	      lo->next = daemon->if_names;
	      daemon->if_names = lo;
	    }
	  else
	    free(lo);
	}
    }
  
  if (addr->sa.sa_family == AF_INET &&
      !iface_check(AF_INET, (union all_addr *)&addr->in.sin_addr, label, &auth_dns))
    return 1;

  if (addr->sa.sa_family == AF_INET6 &&
      !iface_check(AF_INET6, (union all_addr *)&addr->in6.sin6_addr, label, &auth_dns))
    return 1;
    
#ifdef HAVE_DHCP
  /* No DHCP where we're doing auth DNS. */
  if (auth_dns)
    {
      tftp_ok = 0;
      dhcp4_ok = dhcp6_ok = 0;
    }
  else
    for (tmp = daemon->dhcp_except; tmp; tmp = tmp->next)
      if (tmp->name && wildcard_match(tmp->name, ifr.ifr_name))
	{
	  tftp_ok = 0;
	  if (tmp->flags & INAME_4)
	    dhcp4_ok = 0;
	  if (tmp->flags & INAME_6)
	    dhcp6_ok = 0;
	}
#endif
 
  
#ifdef HAVE_TFTP
  if (daemon->tftp_interfaces)
    {
      /* dedicated tftp interface list */
      tftp_ok = 0;
      for (tmp = daemon->tftp_interfaces; tmp; tmp = tmp->next)
	if (tmp->name && wildcard_match(tmp->name, ifr.ifr_name))
	  tftp_ok = 1;
    }
#endif
  
  /* add to list */
  if ((iface = whine_malloc(sizeof(struct irec))))
    {
      int mtu = 0;

      if (ioctl(param->fd, SIOCGIFMTU, &ifr) != -1)
	mtu = ifr.ifr_mtu;

      iface->addr = *addr;
      iface->netmask = netmask;
      iface->tftp_ok = tftp_ok;
      iface->dhcp4_ok = dhcp4_ok;
      iface->dhcp6_ok = dhcp6_ok;
      iface->dns_auth = auth_dns;
      iface->mtu = mtu;
      iface->dad = !!(iface_flags & IFACE_TENTATIVE);
      iface->found = 1;
      iface->done = iface->multicast_done = iface->warned = 0;
      iface->index = if_index;
      iface->label = is_label;
      /************** Pi-hole modification **************/
      if ((iface->slabel = whine_malloc(strlen(label)+1)))
	  strcpy(iface->slabel, label);
      /**************************************************/
      if ((iface->name = whine_malloc(strlen(ifr.ifr_name)+1)))
	{
	  strcpy(iface->name, ifr.ifr_name);
	  iface->next = daemon->interfaces;
	  daemon->interfaces = iface;
	  return 1;
	}
      free(iface);

    }
  
  errno = ENOMEM; 
  return 0;
}

static int iface_allowed_v6(struct in6_addr *local, int prefix, 
			    int scope, int if_index, int flags, 
			    unsigned int preferred, unsigned int valid, void *vparam)
{
  union mysockaddr addr;
  struct in_addr netmask; /* dummy */
  netmask.s_addr = 0;

  (void)scope; /* warning */
  (void)preferred;
  (void)valid;
  
  memset(&addr, 0, sizeof(addr));
#ifdef HAVE_SOCKADDR_SA_LEN
  addr.in6.sin6_len = sizeof(addr.in6);
#endif
  addr.in6.sin6_family = AF_INET6;
  addr.in6.sin6_addr = *local;
  addr.in6.sin6_port = htons(daemon->port);
  /* FreeBSD insists this is zero for non-linklocal addresses */
  if (IN6_IS_ADDR_LINKLOCAL(local))
    addr.in6.sin6_scope_id = if_index;
  else
    addr.in6.sin6_scope_id = 0;
  
  return iface_allowed((struct iface_param *)vparam, if_index, NULL, &addr, netmask, prefix, flags);
}

static int iface_allowed_v4(struct in_addr local, int if_index, char *label,
			    struct in_addr netmask, struct in_addr broadcast, void *vparam)
{
  union mysockaddr addr;
  int prefix, bit;
 
  (void)broadcast; /* warning */

  memset(&addr, 0, sizeof(addr));
#ifdef HAVE_SOCKADDR_SA_LEN
  addr.in.sin_len = sizeof(addr.in);
#endif
  addr.in.sin_family = AF_INET;
  addr.in.sin_addr = local;
  addr.in.sin_port = htons(daemon->port);

  /* determine prefix length from netmask */
  for (prefix = 32, bit = 1; (bit & ntohl(netmask.s_addr)) == 0 && prefix != 0; bit = bit << 1, prefix--);

  return iface_allowed((struct iface_param *)vparam, if_index, label, &addr, netmask, prefix, 0);
}

/*
 * Clean old interfaces no longer found.
 */
static void clean_interfaces(void)
{
  struct irec *iface;
  struct irec **up = &daemon->interfaces;

  for (iface = *up; iface; iface = *up)
  {
    if (!iface->found && !iface->done)
      {
        *up = iface->next;
        free(iface->name);
        free(iface);
      }
    else
      {
        up = &iface->next;
      }
  }
}

/** Release listener if no other interface needs it.
 *
 * @return 1 if released, 0 if still required
 */
static int release_listener(struct listener *l)
{
  if (l->used > 1)
    {
      struct irec *iface;
      for (iface = daemon->interfaces; iface; iface = iface->next)
	if (iface->done && sockaddr_isequal(&l->addr, &iface->addr))
	  {
	    if (iface->found)
	      {
		/* update listener to point to active interface instead */
		if (!l->iface->found)
		  l->iface = iface;
	      }
	    else
	      {
		l->used--;
		iface->done = 0;
	      }
	  }

      /* Someone is still using this listener, skip its deletion */
      if (l->used > 0)
	return 0;
    }

  if (l->iface->done)
    {
      int port;

      port = prettyprint_addr(&l->iface->addr, daemon->addrbuff);
      my_syslog(LOG_DEBUG|MS_DEBUG, _("stopped listening on %s(#%d): %s port %d"),
		l->iface->name, l->iface->index, daemon->addrbuff, port);
      /* In case it ever returns */
      l->iface->done = 0;
      // Pi-hole modification
      log_info("stopped listening on %s(#%d): %s port %d",
	   l->iface->name, l->iface->index, daemon->addrbuff, port);
    }

  if (l->fd != -1)
    close(l->fd);
  if (l->tcpfd != -1)
    close(l->tcpfd);
  if (l->tftpfd != -1)
    close(l->tftpfd);

  free(l);
  return 1;
}

int enumerate_interfaces(int reset)
{
  static struct addrlist *spare = NULL;
  static int done = 0;
  struct iface_param param;
  int errsave, ret = 1;
  struct addrlist *addr, *tmp;
  struct interface_name *intname;
  struct cond_domain *cond;
  struct irec *iface;
#ifdef HAVE_AUTH
  struct auth_zone *zone;
#endif
  struct server *serv;
  
  /* Do this max once per select cycle  - also inhibits netlink socket use
   in TCP child processes. */

  if (reset)
    {
      done = 0;
      return 1;
    }

  if (done)
    return 1;

  done = 1;

  if ((param.fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1)
    return 0;

  /* iface indexes can change when interfaces are created/destroyed. 
     We use them in the main forwarding control path, when the path
     to a server is specified by an interface, so cache them.
     Update the cache here. */
  for (serv = daemon->servers; serv; serv = serv->next)
    if (serv->interface[0] != 0)
      {
#ifdef HAVE_LINUX_NETWORK
	struct ifreq ifr;
	
	safe_strncpy(ifr.ifr_name, serv->interface, IF_NAMESIZE);
	if (ioctl(param.fd, SIOCGIFINDEX, &ifr) != -1) 
	  serv->ifindex = ifr.ifr_ifindex;
#else
	serv->ifindex = if_nametoindex(serv->interface);
#endif
      }
    
again:
  /* Mark interfaces for garbage collection */
  for (iface = daemon->interfaces; iface; iface = iface->next) 
    iface->found = 0;

  /* remove addresses stored against interface_names */
  for (intname = daemon->int_names; intname; intname = intname->next)
    {
      for (addr = intname->addr; addr; addr = tmp)
	{
	  tmp = addr->next;
	  addr->next = spare;
	  spare = addr;
	}
      
      intname->addr = NULL;
    }

  /* remove addresses stored against cond-domains. */
  for (cond = daemon->cond_domain; cond; cond = cond->next)
    {
      for (addr = cond->al; addr; addr = tmp)
	{
	  tmp = addr->next;
	  addr->next = spare;
	  spare = addr;
      }
      
      cond->al = NULL;
    }
  
  /* Remove list of addresses of local interfaces */
  for (addr = daemon->interface_addrs; addr; addr = tmp)
    {
      tmp = addr->next;
      addr->next = spare;
      spare = addr;
    }
  daemon->interface_addrs = NULL;
  
#ifdef HAVE_AUTH
  /* remove addresses stored against auth_zone subnets, but not 
   ones configured as address literals */
  for (zone = daemon->auth_zones; zone; zone = zone->next)
    if (zone->interface_names)
      {
	struct addrlist **up;
	for (up = &zone->subnet, addr = zone->subnet; addr; addr = tmp)
	  {
	    tmp = addr->next;
	    if (addr->flags & ADDRLIST_LITERAL)
	      up = &addr->next;
	    else
	      {
		*up = addr->next;
		addr->next = spare;
		spare = addr;
	      }
	  }
      }
#endif

  param.spare = spare;
  
  ret = iface_enumerate(AF_INET6, &param, (callback_t){.af_inet6=iface_allowed_v6});
  if (ret < 0)
    goto again;
  else if (ret)
    {
      ret = iface_enumerate(AF_INET, &param, (callback_t){.af_inet=iface_allowed_v4});
      if (ret < 0)
	goto again;
    }
 
  errsave = errno;
  close(param.fd);
  
  if (option_bool(OPT_CLEVERBIND))
    { 
      /* Garbage-collect listeners listening on addresses that no longer exist.
	 Does nothing when not binding interfaces or for listeners on localhost, 
	 since the ->iface field is NULL. Note that this needs the protections
	 against reentrancy, hence it's here.  It also means there's a possibility,
	 in OPT_CLEVERBIND mode, that at listener will just disappear after
	 a call to enumerate_interfaces, this is checked OK on all calls. */
      struct listener *l, *tmp, **up;
      int freed = 0;
      
      for (up = &daemon->listeners, l = daemon->listeners; l; l = tmp)
	{
	  tmp = l->next;
	  
	  if (!l->iface || l->iface->found)
	    up = &l->next;
	  else if (release_listener(l))
	    {
	      *up = tmp;
	      freed = 1;
	    }
	}

      if (freed)
	clean_interfaces();
    }

  errno = errsave;
  spare = param.spare;
  
  return ret;
}

/* set NONBLOCK bit on fd: See Stevens 16.6 */
int fix_fd(int fd)
{
  int flags;

  if ((flags = fcntl(fd, F_GETFL)) == -1 ||
      fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    return 0;
  
  return 1;
}

static int make_sock(union mysockaddr *addr, int type, int dienow)
{
  int family = addr->sa.sa_family;
  int fd, rc, opt = 1;
  
  if ((fd = socket(family, type, 0)) == -1)
    {
      int port, errsave;
      char *s;

      /* No error if the kernel just doesn't support this IP flavour */
      if (errno == EPROTONOSUPPORT ||
	  errno == EAFNOSUPPORT ||
	  errno == EINVAL)
	return -1;
      
    err:
      errsave = errno;
      port = prettyprint_addr(addr, daemon->addrbuff);
      if (!option_bool(OPT_NOWILD) && !option_bool(OPT_CLEVERBIND))
	sprintf(daemon->addrbuff, "port %d", port);
      s = _("failed to create listening socket for %s: %s");
      
      if (fd != -1)
	close (fd);
	
      errno = errsave;

      /* Failure to bind addresses given by --listen-address at this point
	 because there's no interface with the address is OK if we're doing bind-dynamic.
	 If/when an interface is created with the relevant address we'll notice
	 and attempt to bind it then. This is in the generic error path so we  close the socket,
	 but EADDRNOTAVAIL is only a possible error from bind() 
	 
	 When a new address is created and we call this code again (dienow == 0) there
	 may still be configured addresses when don't exist, (consider >1 --listen-address,
	 when the first is created, the second will still be missing) so we suppress
	 EADDRNOTAVAIL even in that case to avoid confusing log entries.
      */
      if (!option_bool(OPT_CLEVERBIND) || errno != EADDRNOTAVAIL)
	{
	  if (dienow)
	    die(s, daemon->addrbuff, EC_BADNET);
	  else
	    my_syslog(LOG_WARNING, s, daemon->addrbuff, strerror(errno));
	}
      
      return -1;
    }	
  
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1 || !fix_fd(fd))
    goto err;
  
  if (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) == -1)
    goto err;
  
  if ((rc = bind(fd, (struct sockaddr *)addr, sa_len(addr))) == -1)
    goto err;
  
  if (type == SOCK_STREAM)
    {
#ifdef TCP_FASTOPEN
      int qlen = 5;                           
      setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
#endif
      
      if (listen(fd, TCP_BACKLOG) == -1)
	goto err;
    }
  else if (family == AF_INET)
    {
      if (!option_bool(OPT_NOWILD))
	{
#if defined(HAVE_LINUX_NETWORK) 
	  if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt)) == -1)
	    goto err;
#elif defined(IP_RECVDSTADDR) && defined(IP_RECVIF)
	  if (setsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, &opt, sizeof(opt)) == -1 ||
	      setsockopt(fd, IPPROTO_IP, IP_RECVIF, &opt, sizeof(opt)) == -1)
	    goto err;
#endif
	}
    }
  else if (!set_ipv6pktinfo(fd))
    goto err;
  
  return fd;
}

int set_ipv6pktinfo(int fd)
{
  int opt = 1;

  /* The API changed around Linux 2.6.14 but the old ABI is still supported:
     handle all combinations of headers and kernel.
     OpenWrt note that this fixes the problem addressed by your very broken patch. */
  daemon->v6pktinfo = IPV6_PKTINFO;
  
#ifdef IPV6_RECVPKTINFO
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt, sizeof(opt)) != -1)
    return 1;
# ifdef IPV6_2292PKTINFO
  else if (errno == ENOPROTOOPT && setsockopt(fd, IPPROTO_IPV6, IPV6_2292PKTINFO, &opt, sizeof(opt)) != -1)
    {
      daemon->v6pktinfo = IPV6_2292PKTINFO;
      return 1;
    }
# endif 
#else
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_PKTINFO, &opt, sizeof(opt)) != -1)
    return 1;
#endif

  return 0;
}


/* Find the interface on which a TCP connection arrived, if possible, or zero otherwise. */
int tcp_interface(int fd, int af)
{ 
  (void)fd; /* suppress potential unused warning */
  (void)af; /* suppress potential unused warning */
  int if_index = 0;

#ifdef HAVE_LINUX_NETWORK
  int opt = 1;
  struct cmsghdr *cmptr;
  struct msghdr msg;
  socklen_t len;
  
  /* use mshdr so that the CMSDG_* macros are available */
  msg.msg_control = daemon->packet;
  msg.msg_controllen = len = daemon->packet_buff_sz;

  /* we overwrote the buffer... */
  daemon->srv_save = NULL; 

  if (af == AF_INET)
    {
      if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt)) != -1 &&
	  getsockopt(fd, IPPROTO_IP, IP_PKTOPTIONS, msg.msg_control, &len) != -1)
	{
	  msg.msg_controllen = len;
	  for (cmptr = CMSG_FIRSTHDR(&msg); cmptr; cmptr = CMSG_NXTHDR(&msg, cmptr))
	    if (cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_PKTINFO)
	      {
		union {
		  unsigned char *c;
		  struct in_pktinfo *p;
		} p;
		
		p.c = CMSG_DATA(cmptr);
		if_index = p.p->ipi_ifindex;
	      }
	}
    }
  else
    {
      /* Only the RFC-2292 API has the ability to find the interface for TCP connections,
	 it was removed in RFC-3542 !!!! 

	 Fortunately, Linux kept the 2292 ABI when it moved to 3542. The following code always
	 uses the old ABI, and should work with pre- and post-3542 kernel headers */

#ifdef IPV6_2292PKTOPTIONS   
#  define PKTOPTIONS IPV6_2292PKTOPTIONS
#else
#  define PKTOPTIONS IPV6_PKTOPTIONS
#endif

      if (set_ipv6pktinfo(fd) &&
	  getsockopt(fd, IPPROTO_IPV6, PKTOPTIONS, msg.msg_control, &len) != -1)
	{
          msg.msg_controllen = len;
	  for (cmptr = CMSG_FIRSTHDR(&msg); cmptr; cmptr = CMSG_NXTHDR(&msg, cmptr))
            if (cmptr->cmsg_level == IPPROTO_IPV6 && cmptr->cmsg_type == daemon->v6pktinfo)
              {
                union {
                  unsigned char *c;
                  struct in6_pktinfo *p;
                } p;
                p.c = CMSG_DATA(cmptr);
		
		if_index = p.p->ipi6_ifindex;
              }
	}
    }
#endif /* Linux */
 
  return if_index;
}
      
static struct listener *create_listeners(union mysockaddr *addr, int do_tftp, int dienow)
{
  struct listener *l = NULL;
  int fd = -1, tcpfd = -1, tftpfd = -1;

  (void)do_tftp;

  if (daemon->port != 0)
    {
      fd = make_sock(addr, SOCK_DGRAM, dienow);
      tcpfd = make_sock(addr, SOCK_STREAM, dienow);
    }
  
#ifdef HAVE_TFTP
  if (do_tftp)
    {
      if (addr->sa.sa_family == AF_INET)
	{
	  /* port must be restored to DNS port for TCP code */
	  short save = addr->in.sin_port;
	  addr->in.sin_port = htons(TFTP_PORT);
	  tftpfd = make_sock(addr, SOCK_DGRAM, dienow);
	  addr->in.sin_port = save;
	}
      else
	{
	  short save = addr->in6.sin6_port;
	  addr->in6.sin6_port = htons(TFTP_PORT);
	  tftpfd = make_sock(addr, SOCK_DGRAM, dienow);
	  addr->in6.sin6_port = save;
	}  
    }
#endif

  if (fd != -1 || tcpfd != -1 || tftpfd != -1)
    {
      l = safe_malloc(sizeof(struct listener));
      l->next = NULL;
      l->fd = fd;
      l->tcpfd = tcpfd;
      l->tftpfd = tftpfd;
      l->addr = *addr;
      l->used = 1;
      l->iface = NULL;
    }

    // Pi-hole modification
    const int port = prettyprint_addr(addr, daemon->addrbuff);
    log_info("listening on %s port %d", daemon->addrbuff, port);

  return l;
}

void create_wildcard_listeners(void)
{
  union mysockaddr addr;
  struct listener *l, *l6;

  memset(&addr, 0, sizeof(addr));
#ifdef HAVE_SOCKADDR_SA_LEN
  addr.in.sin_len = sizeof(addr.in);
#endif
  addr.in.sin_family = AF_INET;
  addr.in.sin_addr.s_addr = INADDR_ANY;
  addr.in.sin_port = htons(daemon->port);

  l = create_listeners(&addr, !!option_bool(OPT_TFTP), 1);

  memset(&addr, 0, sizeof(addr));
#ifdef HAVE_SOCKADDR_SA_LEN
  addr.in6.sin6_len = sizeof(addr.in6);
#endif
  addr.in6.sin6_family = AF_INET6;
  addr.in6.sin6_addr = in6addr_any;
  addr.in6.sin6_port = htons(daemon->port);
 
  l6 = create_listeners(&addr, !!option_bool(OPT_TFTP), 1);
  if (l) 
    l->next = l6;
  else 
    l = l6;

  daemon->listeners = l;
}

static struct listener *find_listener(union mysockaddr *addr)
{
  struct listener *l;
  for (l = daemon->listeners; l; l = l->next)
    if (sockaddr_isequal(&l->addr, addr))
      return l;
  return NULL;
}

void create_bound_listeners(int dienow)
{
  struct listener *new;
  struct irec *iface;
  struct iname *if_tmp;
  struct listener *existing;

  for (iface = daemon->interfaces; iface; iface = iface->next)
    if (!iface->done && !iface->dad && iface->found)
      {
	existing = find_listener(&iface->addr);
	if (existing)
	  {
	    iface->done = 1;
	    existing->used++; /* increase usage counter */
	  }
	else if ((new = create_listeners(&iface->addr, iface->tftp_ok, dienow)))
	  {
	    new->iface = iface;
	    new->next = daemon->listeners;
	    daemon->listeners = new;
	    iface->done = 1;

	    /* Don't log the initial set of listen addresses created
               at startup, since this is happening before the logging
               system is initialised and the sign-on printed. */
            if (!dienow)
              {
		int port = prettyprint_addr(&iface->addr, daemon->addrbuff);
		my_syslog(LOG_DEBUG|MS_DEBUG, _("listening on %s(#%d): %s port %d"),
			  iface->name, iface->index, daemon->addrbuff, port);
	      }
	    // Pi-hole modification
	    const int port = prettyprint_addr(&iface->addr, daemon->addrbuff);
	    log_info("listening on %s(#%d): %s port %d",
		     iface->name, iface->index, daemon->addrbuff, port);
	  }
      }

  /* Check for --listen-address options that haven't been used because there's
     no interface with a matching address. These may be valid: eg it's possible
     to listen on 127.0.1.1 even if the loopback interface is 127.0.0.1

     If the address isn't valid the bind() will fail and we'll die() 
     (except in bind-dynamic mode, when we'll complain but keep trying.)

     The resulting listeners have the ->iface field NULL, and this has to be
     handled by the DNS and TFTP code. It disables --localise-queries processing
     (no netmask) and some MTU login the tftp code. */

  for (if_tmp = daemon->if_addrs; if_tmp; if_tmp = if_tmp->next)
    if (!(if_tmp->flags & INAME_USED) && 
	(new = create_listeners(&if_tmp->addr, !!option_bool(OPT_TFTP), dienow)))
      {
	new->next = daemon->listeners;
	daemon->listeners = new;

	if (!dienow)
	  {
	    int port = prettyprint_addr(&if_tmp->addr, daemon->addrbuff);
	    my_syslog(LOG_DEBUG|MS_DEBUG, _("listening on %s port %d"), daemon->addrbuff, port);
	  }
	// Pi-hole modification
	const int port = prettyprint_addr(&if_tmp->addr, daemon->addrbuff);
	log_info("listening on %s port %d", daemon->addrbuff, port);
      }
}

/* In --bind-interfaces, the only access control is the addresses we're listening on. 
   There's nothing to avoid a query to the address of an internal interface arriving via
   an external interface where we don't want to accept queries, except that in the usual 
   case the addresses of internal interfaces are RFC1918. When bind-interfaces in use, 
   and we listen on an address that looks like it's probably globally routeable, shout.

   The fix is to use --bind-dynamic, which actually checks the arrival interface too.
   Tough if your platform doesn't support this.

   Note that checking the arrival interface is supported in the standard IPv6 API and
   always done, so we don't warn about any IPv6 addresses here.
*/

void warn_bound_listeners(void)
{
  struct irec *iface; 	
  int advice = 0;

  for (iface = daemon->interfaces; iface; iface = iface->next)
    if (!iface->dns_auth)
      {
	if (iface->addr.sa.sa_family == AF_INET)
	  {
	    if (!private_net(iface->addr.in.sin_addr, 1))
	      {
		inet_ntop(AF_INET, &iface->addr.in.sin_addr, daemon->addrbuff, ADDRSTRLEN);
		iface->warned = advice = 1;
		my_syslog(LOG_WARNING, 
			  _("LOUD WARNING: listening on %s may accept requests via interfaces other than %s"),
			  daemon->addrbuff, iface->name);
	      }
	  }
      }
  
  if (advice)
    my_syslog(LOG_WARNING, _("LOUD WARNING: use --bind-dynamic rather than --bind-interfaces to avoid DNS amplification attacks via these interface(s)")); 
}

void warn_wild_labels(void)
{
  struct irec *iface;

  for (iface = daemon->interfaces; iface; iface = iface->next)
    if (iface->found && iface->name && iface->label)
      my_syslog(LOG_WARNING, _("warning: using interface %s instead"), iface->name);
}

void warn_int_names(void)
{
  struct interface_name *intname;
 
  for (intname = daemon->int_names; intname; intname = intname->next)
    if (!intname->addr)
      my_syslog(LOG_WARNING, _("warning: no addresses found for interface %s"), intname->intr);
}
 
int is_dad_listeners(void)
{
  struct irec *iface;
  
  if (option_bool(OPT_NOWILD))
    for (iface = daemon->interfaces; iface; iface = iface->next)
      if (iface->dad && !iface->done)
	return 1;
  
  return 0;
}

#ifdef HAVE_DHCP6
void join_multicast(int dienow)      
{
  struct irec *iface, *tmp;

  for (iface = daemon->interfaces; iface; iface = iface->next)
    if (iface->addr.sa.sa_family == AF_INET6 && iface->dhcp6_ok && !iface->multicast_done)
      {
	/* There's an irec per address but we only want to join for multicast 
	   once per interface. Weed out duplicates. */
	for (tmp = daemon->interfaces; tmp; tmp = tmp->next)
	  if (tmp->multicast_done && tmp->index == iface->index)
	    break;
	
	iface->multicast_done = 1;
	
	if (!tmp)
	  {
	    struct ipv6_mreq mreq;
	    int err = 0;

	    mreq.ipv6mr_interface = iface->index;
	    
	    inet_pton(AF_INET6, ALL_RELAY_AGENTS_AND_SERVERS, &mreq.ipv6mr_multiaddr);
	    
	    if ((daemon->doing_dhcp6 || daemon->relay6) &&
		setsockopt(daemon->dhcp6fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == -1)
	      err = errno;
	    
	    inet_pton(AF_INET6, ALL_SERVERS, &mreq.ipv6mr_multiaddr);
	    
	    if (daemon->doing_dhcp6 && 
		setsockopt(daemon->dhcp6fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == -1)
	      err = errno;
	    
	    inet_pton(AF_INET6, ALL_ROUTERS, &mreq.ipv6mr_multiaddr);
	    
	    if (daemon->doing_ra &&
		setsockopt(daemon->icmp6fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == -1)
	      err = errno;
	    
	    if (err)
	      {
		char *s = _("interface %s failed to join DHCPv6 multicast group: %s");
		errno = err;

#ifdef HAVE_LINUX_NETWORK
		if (errno == ENOMEM)
		  my_syslog(LOG_ERR, _("try increasing /proc/sys/net/core/optmem_max"));
#endif

		if (dienow)
		  die(s, iface->name, EC_BADNET);
		else
		  my_syslog(LOG_ERR, s, iface->name, strerror(errno));
	      }
	  }
      }
}
#endif

int local_bind(int fd, union mysockaddr *addr, char *intname, unsigned int ifindex, int is_tcp)
{
  union mysockaddr addr_copy = *addr;
  unsigned short port;
  int tries = 1;
  unsigned short ports_avail = 1;

  if (addr_copy.sa.sa_family == AF_INET)
    port = addr_copy.in.sin_port;
  else
    port = addr_copy.in6.sin6_port;

  /* cannot set source _port_ for TCP connections. */
  if (is_tcp)
    port = 0;
  else if (port == 0 && daemon->max_port != 0 && daemon->max_port >= daemon->min_port)
    {
      /* Bind a random port within the range given by min-port and max-port if either
	 or both are set. Otherwise use the OS's random ephemeral port allocation by
	 leaving port == 0 and tries == 1 */
      ports_avail = daemon->max_port - daemon->min_port + 1;
      tries =  (ports_avail < SMALL_PORT_RANGE) ? ports_avail : 100;
      port = htons(daemon->min_port + (rand16() % ports_avail));
    }
  
  while (1)
    {
      /* elide bind() call if it's to port 0, address 0 */
      if (addr_copy.sa.sa_family == AF_INET)
	{
	  if (port == 0 && addr_copy.in.sin_addr.s_addr == 0)
	    break;
	  addr_copy.in.sin_port = port;
	}
      else
	{
	  if (port == 0 && IN6_IS_ADDR_UNSPECIFIED(&addr_copy.in6.sin6_addr))
	    break;
	  addr_copy.in6.sin6_port = port;
	}
      
      if (bind(fd, (struct sockaddr *)&addr_copy, sa_len(&addr_copy)) != -1)
	break;
      
       if (errno != EADDRINUSE && errno != EACCES) 
	 return 0;

      if (--tries == 0)
	return 0;

      /* For small ranges, do a systematic search, not a random one. */
      if (ports_avail < SMALL_PORT_RANGE)
	{
	  unsigned short hport = ntohs(port);
	  if (hport++ == daemon->max_port)
	    hport = daemon->min_port;
	  port = htons(hport);
	}
      else
	port = htons(daemon->min_port + (rand16() % ports_avail));
    }

  if (!is_tcp && ifindex > 0)
    {
#if defined(IP_UNICAST_IF)
      if (addr_copy.sa.sa_family == AF_INET)
        {
          uint32_t ifindex_opt = htonl(ifindex);
          return setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, &ifindex_opt, sizeof(ifindex_opt)) == 0;
        }
#endif
#if defined (IPV6_UNICAST_IF)
      if (addr_copy.sa.sa_family == AF_INET6)
        {
          uint32_t ifindex_opt = htonl(ifindex);
          return setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, &ifindex_opt, sizeof(ifindex_opt)) == 0;
        }
#endif
    }

  (void)intname; /* suppress potential unused warning */
#if defined(SO_BINDTODEVICE)
  if (intname[0] != 0 &&
      setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, intname, IF_NAMESIZE) == -1)
    return 0;
#endif

  return 1;
}

static struct serverfd *allocate_sfd(union mysockaddr *addr, char *intname, unsigned int ifindex)
{
  struct serverfd *sfd;
  int errsave;
  int opt = 1;
  
  /* when using random ports, servers which would otherwise use
     the INADDR_ANY/port0 socket have sfd set to NULL, this is 
     anything without an explictly set source port. */
  if (!daemon->osport)
    {
      errno = 0;
      
      if (addr->sa.sa_family == AF_INET &&
	  addr->in.sin_port == htons(0)) 
	return NULL;

      if (addr->sa.sa_family == AF_INET6 &&
	  addr->in6.sin6_port == htons(0)) 
	return NULL;
    }

  /* may have a suitable one already */
  for (sfd = daemon->sfds; sfd; sfd = sfd->next )
    if (ifindex == sfd->ifindex &&
	sockaddr_isequal(&sfd->source_addr, addr) &&
	strcmp(intname, sfd->interface) == 0)
      return sfd;
  
  /* need to make a new one. */
  errno = ENOMEM; /* in case malloc fails. */
  if (!(sfd = whine_malloc(sizeof(struct serverfd))))
    return NULL;
  
  if ((sfd->fd = socket(addr->sa.sa_family, SOCK_DGRAM, 0)) == -1)
    {
      free(sfd);
      return NULL;
    }

  if ((addr->sa.sa_family == AF_INET6 && setsockopt(sfd->fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) == -1) ||
      !local_bind(sfd->fd, addr, intname, ifindex, 0) || !fix_fd(sfd->fd))
    { 
      errsave = errno; /* save error from bind/setsockopt. */
      close(sfd->fd);
      free(sfd);
      errno = errsave;
      return NULL;
    }

  safe_strncpy(sfd->interface, intname, sizeof(sfd->interface)); 
  sfd->source_addr = *addr;
  sfd->next = daemon->sfds;
  sfd->ifindex = ifindex;
  sfd->preallocated = 0;
  daemon->sfds = sfd;

  return sfd; 
}

/* create upstream sockets during startup, before root is dropped which may be needed
   this allows query_port to be a low port and interface binding */
void pre_allocate_sfds(void)
{
  struct server *srv;
  struct serverfd *sfd;
  
  if (daemon->query_port != 0)
    {
      union  mysockaddr addr;
      memset(&addr, 0, sizeof(addr));
      addr.in.sin_family = AF_INET;
      addr.in.sin_addr.s_addr = INADDR_ANY;
      addr.in.sin_port = htons(daemon->query_port);
#ifdef HAVE_SOCKADDR_SA_LEN
      addr.in.sin_len = sizeof(struct sockaddr_in);
#endif
      if ((sfd = allocate_sfd(&addr, "", 0)))
	sfd->preallocated = 1;

      memset(&addr, 0, sizeof(addr));
      addr.in6.sin6_family = AF_INET6;
      addr.in6.sin6_addr = in6addr_any;
      addr.in6.sin6_port = htons(daemon->query_port);
#ifdef HAVE_SOCKADDR_SA_LEN
      addr.in6.sin6_len = sizeof(struct sockaddr_in6);
#endif
      if ((sfd = allocate_sfd(&addr, "", 0)))
	sfd->preallocated = 1;
    }
  
  for (srv = daemon->servers; srv; srv = srv->next)
    if (!allocate_sfd(&srv->source_addr, srv->interface, srv->ifindex) &&
	errno != 0 &&
	option_bool(OPT_NOWILD))
      {
	(void)prettyprint_addr(&srv->source_addr, daemon->namebuff);
	if (srv->interface[0] != 0)
	  {
	    strcat(daemon->namebuff, " ");
	    strcat(daemon->namebuff, srv->interface);
	  }
	die(_("failed to bind server socket for %s: %s"),
	    daemon->namebuff, EC_BADNET);
      }  
}

void check_servers(int no_loop_check)
{
  struct irec *iface;
  struct server *serv;
  struct serverfd *sfd, *tmp, **up;
  int port = 0, count;
  int locals = 0;

  (void)no_loop_check;
  
#ifdef HAVE_LOOP
  if (!no_loop_check)
    loop_send_probes();
#endif

  /* clear all marks. */
  mark_servers(0);
  
 /* interface may be new since startup */
  if (!option_bool(OPT_NOWILD))
    enumerate_interfaces(0);

  /* don't garbage collect pre-allocated sfds. */
  for (sfd = daemon->sfds; sfd; sfd = sfd->next)
    sfd->used = sfd->preallocated;

  for (count = 0, serv = daemon->servers; serv; serv = serv->next)
    {
      port = prettyprint_addr(&serv->addr, daemon->namebuff);
      
      /* 0.0.0.0 is nothing, the stack treats it like 127.0.0.1 */
      if (serv->addr.sa.sa_family == AF_INET &&
	  serv->addr.in.sin_addr.s_addr == 0)
	{
	  serv->flags |= SERV_MARK;
	  continue;
	}
      
      for (iface = daemon->interfaces; iface; iface = iface->next)
	if (sockaddr_isequal(&serv->addr, &iface->addr))
	  break;
      if (iface)
	{
	  my_syslog(LOG_WARNING, _("ignoring nameserver %s - local interface"), daemon->namebuff);
	  serv->flags |= SERV_MARK;
	  continue;
	}
      
      /* Do we need a socket set? */
      if (!serv->sfd && 
	  !(serv->sfd = allocate_sfd(&serv->source_addr, serv->interface, serv->ifindex)) &&
	  errno != 0)
	{
	  my_syslog(LOG_WARNING, 
		    _("ignoring nameserver %s - cannot make/bind socket: %s"),
		    daemon->namebuff, strerror(errno));
	  serv->flags |= SERV_MARK;
	  continue;
	}
      
      if (serv->sfd)
	serv->sfd->used = 1;
      
      if (count == SERVERS_LOGGED)
	my_syslog(LOG_INFO, _("more servers are defined but not logged"));
      
      if (++count > SERVERS_LOGGED)
	continue;
      
      if (strlen(serv->domain) != 0 || (serv->flags & SERV_FOR_NODOTS))
	{
	  char *s1, *s2, *s3 = "", *s4 = "";

	  if (serv->flags & SERV_FOR_NODOTS)
	    s1 = _("unqualified"), s2 = _("names");
	  else if (strlen(serv->domain) == 0)
	    s1 = _("default"), s2 = "";
	  else
	    s1 = _("domain"), s2 = serv->domain, s4 = (serv->flags & SERV_WILDCARD) ? "*" : "";
	  
	  my_syslog(LOG_INFO, _("using nameserver %s#%d for %s %s%s %s"), daemon->namebuff, port, s1, s4, s2, s3);
	}
#ifdef HAVE_LOOP
      else if (serv->flags & SERV_LOOP)
	my_syslog(LOG_INFO, _("NOT using nameserver %s#%d - query loop detected"), daemon->namebuff, port); 
#endif
      else if (serv->interface[0] != 0)
	my_syslog(LOG_INFO, _("using nameserver %s#%d(via %s)"), daemon->namebuff, port, serv->interface); 
      else
	my_syslog(LOG_INFO, _("using nameserver %s#%d"), daemon->namebuff, port); 

    }
  
  for (count = 0, serv = daemon->local_domains; serv; serv = serv->next)
    {
       if (++count > SERVERS_LOGGED)
	 continue;
       
       if ((serv->flags & SERV_LITERAL_ADDRESS) &&
	   !(serv->flags & (SERV_6ADDR | SERV_4ADDR | SERV_ALL_ZEROS)) &&
	   strlen(serv->domain))
	 {
	   count--;
	   if (++locals <= LOCALS_LOGGED)
	     my_syslog(LOG_INFO, _("using only locally-known addresses for %s"), serv->domain);
	 }
       else if (serv->flags & SERV_USE_RESOLV && serv->domain_len != 0)
	 my_syslog(LOG_INFO, _("using standard nameservers for %s"), serv->domain);
    }
  
  if (locals > LOCALS_LOGGED)
    my_syslog(LOG_INFO, _("using %d more local addresses"), locals - LOCALS_LOGGED);
  if (count - 1 > SERVERS_LOGGED)
    my_syslog(LOG_INFO, _("using %d more nameservers"), count - SERVERS_LOGGED - 1);

  /* Remove unused sfds */
  for (sfd = daemon->sfds, up = &daemon->sfds; sfd; sfd = tmp)
    {
       tmp = sfd->next;
       if (!sfd->used) 
	{
	  *up = sfd->next;
	  close(sfd->fd);
	  free(sfd);
	} 
      else
	up = &sfd->next;
    }
  
  cleanup_servers(); /* remove servers we just deleted. */
  build_server_array(); 
}

/* Return zero if no servers found, in that case we keep polling.
   This is a protection against an update-time/write race on resolv.conf */
int reload_servers(char *fname)
{
  FILE *f;
  char *line;
  int gotone = 0;

  /* buff happens to be MAXDNAME long... */
  if (!(f = fopen(fname, "r")))
    {
      my_syslog(LOG_ERR, _("failed to read %s: %s"), fname, strerror(errno));
      return 0;
    }
   
  mark_servers(SERV_FROM_RESOLV);
    
  while ((line = fgets(daemon->namebuff, MAXDNAME, f)))
    {
      union mysockaddr addr, source_addr;
      char *token = strtok(line, " \t\n\r");
      
      if (!token)
	continue;
      if (strcmp(token, "nameserver") != 0 && strcmp(token, "server") != 0)
	continue;
      if (!(token = strtok(NULL, " \t\n\r")))
	continue;
      
      memset(&addr, 0, sizeof(addr));
      memset(&source_addr, 0, sizeof(source_addr));
      
      if (inet_pton(AF_INET, token, &addr.in.sin_addr) > 0)
	{
#ifdef HAVE_SOCKADDR_SA_LEN
	  source_addr.in.sin_len = addr.in.sin_len = sizeof(source_addr.in);
#endif
	  source_addr.in.sin_family = addr.in.sin_family = AF_INET;
	  addr.in.sin_port = htons(NAMESERVER_PORT);
	  source_addr.in.sin_addr.s_addr = INADDR_ANY;
	  source_addr.in.sin_port = htons(daemon->query_port);
	}
      else 
	{	
	  int scope_index = 0;
	  char *scope_id = strchr(token, '%');
	  
	  if (scope_id)
	    {
	      *(scope_id++) = 0;
	      scope_index = if_nametoindex(scope_id);
	    }
	  
	  if (inet_pton(AF_INET6, token, &addr.in6.sin6_addr) > 0)
	    {
#ifdef HAVE_SOCKADDR_SA_LEN
	      source_addr.in6.sin6_len = addr.in6.sin6_len = sizeof(source_addr.in6);
#endif
	      source_addr.in6.sin6_family = addr.in6.sin6_family = AF_INET6;
	      source_addr.in6.sin6_flowinfo = addr.in6.sin6_flowinfo = 0;
	      addr.in6.sin6_port = htons(NAMESERVER_PORT);
	      addr.in6.sin6_scope_id = scope_index;
	      source_addr.in6.sin6_addr = in6addr_any;
	      source_addr.in6.sin6_port = htons(daemon->query_port);
	      source_addr.in6.sin6_scope_id = 0;
	    }
	  else
	    continue;
	}

      add_update_server(SERV_FROM_RESOLV, &addr, &source_addr, NULL, NULL, NULL);
      gotone = 1;
    }
  
  fclose(f);
  cleanup_servers();

  return gotone;
}

/* Called when addresses are added or deleted from an interface */
void newaddress(time_t now)
{
#ifdef HAVE_DHCP
  struct dhcp_relay *relay;
#endif
  
  (void)now;
  
  if (option_bool(OPT_CLEVERBIND) || option_bool(OPT_LOCAL_SERVICE) ||
      daemon->doing_dhcp6 || daemon->relay6 || daemon->doing_ra)
    enumerate_interfaces(0);
  
  if (option_bool(OPT_CLEVERBIND))
    create_bound_listeners(0);

#ifdef HAVE_DHCP
  /* clear cache of subnet->relay index */
  for (relay = daemon->relay4; relay; relay = relay->next)
    relay->iface_index = 0;
#endif
  
#ifdef HAVE_DHCP6
  if (daemon->doing_dhcp6 || daemon->relay6 || daemon->doing_ra)
    join_multicast(0);
  
  if (daemon->doing_dhcp6 || daemon->doing_ra)
    dhcp_construct_contexts(now);
  
  if (daemon->doing_dhcp6)
    lease_find_interfaces(now);

  for (relay = daemon->relay6; relay; relay = relay->next)
    relay->iface_index = 0;
#endif
}
