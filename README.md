# SSH Bouncer

A fake SSH daemon which runs on any number of ports, listens for clients,
sends them a fake SSH version infos and then keeps the connections open forever
to simulate a non-responsive SSH daemon.

This is useful when you run sshd on a non-default port but want to pretend
that SSH is running on a standard port.


## Configration

You can configure the following things:

 - Ports the daemon should listen to (`sb_listen_config`)
 - Fake version strings to send to clients (`sb_listen_config`)
 - User and group id which the daemon will run as (`sb_userid`)
 - chroot folder to switch to (`sb_chroot`)
 
Edit the first few lines if `ssh-bouncer.c` and then re-compile to do this.


## License

This daemon is MIT licensed (see `ssh-bouncer.c`), copyright &copy; 2012 Torsten Becker <torsten.becker@gmail.com>.
