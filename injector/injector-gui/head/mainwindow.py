import gtk
from iface_browser import InterfaceBrowser
import help_box
from gui import policy
from dialog import Dialog
from model import stable, testing, network_levels, SafeException
from freshness import freshness_levels, Freshness

class MainWindow(Dialog):
	progress = None
	browser = None

	def __init__(self, prog_args):
		Dialog.__init__(self)
		self.set_title('Dependency Injector')
		self.set_default_size(400, 300)

		tips = gtk.Tooltips()

		# Network use
		hbox = gtk.HBox(False, 2)
		self.vbox.pack_start(hbox, False, True, 0)
		hbox.set_border_width(4)

		network = gtk.combo_box_new_text()
		for level in network_levels:
			network.append_text(level.capitalize())
		network.set_active(list(network_levels).index(policy.network_use))
		hbox.pack_start(gtk.Label('Network use:'), False, True, 0)
		hbox.pack_start(network, True, True, 2)
		def set_network_use(combo):
			policy.network_use = network_levels[network.get_active()]
			policy.save_config()
			policy.recalculate()
		network.connect('changed', set_network_use)

		hbox.show_all()

		# Freshness
		hbox = gtk.HBox(False, 2)
		self.vbox.pack_start(hbox, False, True, 0)
		hbox.set_border_width(4)

		times = [x.time for x in freshness_levels]
		if policy.freshness not in times:
			freshness_levels.append(Freshness(policy.freshness,
							  '%d seconds' % policy.freshness))
			times.append(policy.freshness)
		freshness = gtk.combo_box_new_text()
		for level in freshness_levels:
			freshness.append_text(str(level))
		freshness.set_active(times.index(policy.freshness))
		hbox.pack_start(gtk.Label('Freshness:'), False, True, 0)
		hbox.pack_start(freshness, True, True, 2)
		def set_freshness(combo):
			policy.freshness = freshness_levels[freshness.get_active()].time
			policy.save_config()
			policy.recalculate()
		freshness.connect('changed', set_freshness)

		button = gtk.Button('Refresh all now')
		def refresh_all(b):
			for x in policy.walk_interfaces():
				policy.begin_iface_download(x, True)
		button.connect('clicked', refresh_all)
		hbox.pack_start(button, False, True, 2)

		hbox.show_all()

		# Tree view
		self.browser = InterfaceBrowser()
		self.vbox.pack_start(self.browser, True, True, 0)
		self.browser.show()

		# Select versions
		hbox = gtk.HBox(False, 2)
		self.vbox.pack_start(hbox, False, True, 0)
		hbox.set_border_width(4)

		button = gtk.Button()
		self.browser.edit_properties.connect_proxy(button)
		hbox.pack_start(button, False, True, 0)

		stable_toggle = gtk.CheckButton('Help test new versions')
		hbox.pack_start(stable_toggle, False, True, 0)
		tips.set_tip(stable_toggle,
			"Try out new versions as soon as they are available, instead of "
			"waiting for them to be marked as 'stable'. "
			"This sets the default policy. Click on 'Interface Properties...' "
			"to set the policy for an individual interface.")
		stable_toggle.set_active(policy.help_with_testing)
		def toggle_stability(toggle):
			policy.help_with_testing = toggle.get_active()
			policy.save_config()
			policy.recalculate()
		stable_toggle.connect('toggled', toggle_stability)

		hbox.show_all()

		# Progress bar
		self.progress = gtk.ProgressBar()
		self.vbox.pack_start(self.progress, False, True, 0)

		# Responses

		self.add_button(gtk.STOCK_HELP, gtk.RESPONSE_HELP)
		self.add_button(gtk.STOCK_CANCEL, gtk.RESPONSE_CANCEL)
		self.add_button(gtk.STOCK_EXECUTE, gtk.RESPONSE_OK)
		self.set_default_response(gtk.RESPONSE_OK)

		def response(dialog, resp):
			if resp == gtk.RESPONSE_CANCEL:
				self.destroy()
			elif resp == gtk.RESPONSE_OK:
				import download_box
				download_box.download_and_run(self, prog_args)
			elif resp == gtk.RESPONSE_HELP:
				gui_help.display()
		self.connect('response', response)

gui_help = help_box.HelpBox("Injector Help",
('Overview', """
A program is made up of many different components, typically written by different \
groups of people. Each component is available in multiple versions. The injector is \
used when starting a program. Its job is to decide which implementation of each required \
component to use.

An interface describes what a component does. The injector starts with \
the interface for the program you want to run (like 'The Gimp') and chooses an \
implementation (like 'The Gimp 2.2.0'). However, this implementation \
will in turn depend on other interfaces, such as 'GTK' (which draws the menus \
and buttons). Thus, the injector must choose implementations of \
each dependancy (each of which may require further interfaces, and so on)."""),

('List of interfaces', """
The main window displays all these interfaces, and the version of each chosen \
implementation. The top-most one represents the program you tried to run, and each direct \
child is a dependancy.

If you are happy with the choices shown, click on the Execute button to run the \
program."""),

('Choosing different versions', """
There are three ways to control which implementations are chosen. You can adjust the \
network policy and the overall stability policy, which affect all interfaces, or you \
can edit the policy of individual interfaces.

The 'Network use' option controls how the injector uses the network. If off-line, \
the network is not used at all. If 'Minimal' is selected then the injector will use \
the network if needed, but only if it has no choice. It will run an out-of-date \
version rather than download a newer one. If 'Full' is selected, the injector won't \
worry about how much it downloads, but will always pick the version it thinks is best.

The overall stability policy can either be to prefer stable versions, or to help test \
new versions. Choose whichever suits you. Since different programmers have different \
ideas of what 'stable' means, you may wish to override this on a per-interface basis \
(see below).

To set the policy for an interface individually, select it and click on 'Interface \
Properties'. See that dialog's help text for more information."""))
