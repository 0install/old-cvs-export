<?xml version="1.0"?>

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
		xmlns='http://www.w3.org/1999/xhtml'>

<!-- doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd" -->
  <xsl:output method="xml" encoding="utf-8"
	doctype-system="/usr/share/4Suite/Schemata/xhtml1-strict.dtd"
	doctype-public="-//W3C//DTD XHTML 1.0 Strict//EN"/>

  <xsl:param name="file">unknown</xsl:param>

  <xsl:template name='page'>
    <xsl:param name="base">unknown</xsl:param>
    <span>
      <xsl:if test='$file = concat($base, ".html")'>
        <xsl:attribute name='class'>selected</xsl:attribute>
      </xsl:if>
      <xsl:text>&#160;</xsl:text>
      <a href="{$base}.html"><xsl:value-of select='$label'/></a>
      <xsl:text>&#160;</xsl:text>
    </span>
  </xsl:template>

  <xsl:template name='make-links'>
<xsl:text>
</xsl:text>
    <div class='pages'>
      <xsl:for-each select='document("structure.xml")/layout/item'>
        <xsl:call-template name='page'>
          <xsl:with-param name='base'><xsl:value-of select='@base'/></xsl:with-param>
          <xsl:with-param name='label'><xsl:value-of select='@label'/></xsl:with-param>
        </xsl:call-template>
      </xsl:for-each>
    </div>
    <!-- second level navigation -->
    <xsl:for-each select='document("structure.xml")/layout/item[item]'>
      <xsl:if test='starts-with($file, @base)'>
        <div class='pages'>
          <xsl:for-each select='item'>
            <xsl:call-template name='page'>
              <xsl:with-param name='base'><xsl:value-of select='@base'/></xsl:with-param>
              <xsl:with-param name='label'><xsl:value-of select='@label'/></xsl:with-param>
            </xsl:call-template>
          </xsl:for-each>
	</div>
      </xsl:if>
    </xsl:for-each>
<xsl:text>
</xsl:text>
  </xsl:template>

  <xsl:template match='/*'>
    <html xml:lang='en' lang='en'>
      <head>
        <link rel='stylesheet' type='text/css' href='style.css' />
        <title>The Zero Install system</title>
      </head>

      <body>
        <div class='heading' style='border-bottom: 2px dashed black'>
	  <div style='float:left'>
	    <a href="http://sourceforge.net/projects/zero-install">
	      <img width="88" height="31" alt="SF logo"
     	       src="http://sourceforge.net/sflogo.php?group_id=7023&amp;type=1"/>
	    </a>
	  </div>
          <div style='float:right'>
	    <a href="http://jigsaw.w3.org/css-validator/check/referer">
	      <img style="border:0;width:88px;height:31px"
	   		src="http://jigsaw.w3.org/css-validator/images/vcss" 
	    		alt="Valid CSS!"/>
	    </a>
	    <a class='outside' href="http://validator.w3.org/check/referer">
	      <img src="http://www.w3.org/Icons/valid-xhtml10"
	    	   alt="Valid XHTML 1.0!" height="31" width="88"/>
	    </a>
	  </div>

	  <h1>The Zero Install system</h1>
          <p class='author'>Thomas Leonard [ <a href="support.html">contact</a> | <a href="public_key.gpg">GPG public key</a> | <a href="http://sourceforge.net/developer/user_donations.php?user_id=40461">donations</a> ]</p>

	  <xsl:call-template name='make-links'/>
        </div>
        <div class='main'>
          <xsl:apply-templates/>
        </div>
	
        <div class='heading' style='border-top: 2px dashed black'>
	  <xsl:call-template name='make-links'/>
	  <p class='thanks'>
Thanks to the University of Southampton for the 0install.org, 0install.net, zero-install.org and zero-install.net domain names!
	  </p>
	</div>
      </body>
    </html>
  </xsl:template>
  
  <xsl:template match='@*|node()'>
    <xsl:copy>
      <xsl:apply-templates select='@*|node()'/>
    </xsl:copy>
  </xsl:template>

  <xsl:template match='*[name() = "toc"]'>
    <ol>
    <xsl:for-each select='//*[name() = "h3"]'>
      <li><a href="#{generate-id()}"><xsl:value-of select='.'/></a></li>
    </xsl:for-each>
    </ol>
  </xsl:template>

  <xsl:template match='*[name() = "h3"]'>
    <xsl:copy>
      <xsl:attribute name='id'><xsl:value-of select="generate-id()"/></xsl:attribute>
      <xsl:apply-templates/>
    </xsl:copy>
  </xsl:template>

</xsl:stylesheet>
