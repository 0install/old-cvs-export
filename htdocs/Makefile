HTML = index.html compare.html faq.html install.html technical.html support.html docs.html security.html packagers.html packagers-cs-1.html packagers-cs-2.html injector.html injector-packagers.html injector-design.html injector-security.html injector-using.html injector-tests.html

all: ${HTML}

%.html: %.xml to_html.xsl structure.xml
	xsltproc -o $@ --stringparam file "$@" to_html.xsl "$<"
	xmllint --noout --valid $@

linklint:
	linklint -error -warn -xref -forward -http -host 0install.net /@
