<?xml version="1.0"?>

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
		xmlns='http://www.w3.org/1999/xhtml'>

  <xsl:output method="xml" encoding="utf-8"
	doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"
	doctype-public="-//W3C//DTD XHTML 1.0 Strict//EN"/>

  <xsl:param name="file">unknown</xsl:param>

  <xsl:template name='page'>
    <xsl:param name="href">unknown</xsl:param>
    <span>
      <xsl:if test='$file = $href'>
        <xsl:attribute name='class'>selected</xsl:attribute>
      </xsl:if>
      <xsl:text>&#160;</xsl:text>
      <a href="{$href}"><xsl:value-of select='$label'/></a>
      <xsl:text>&#160;</xsl:text>
    </span>
  </xsl:template>

  <xsl:template name='make-links'>
<xsl:text>
</xsl:text>
    <div class='pages'>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>index.html</xsl:with-param>
        <xsl:with-param name='label'>Overview</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>install.html</xsl:with-param>
        <xsl:with-param name='label'>Installation</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>compare.html</xsl:with-param>
        <xsl:with-param name='label'>Motivation</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>faq.html</xsl:with-param>
        <xsl:with-param name='label'>FAQ</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>docs.html</xsl:with-param>
        <xsl:with-param name='label'>Documentation</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>packagers.html</xsl:with-param>
        <xsl:with-param name='label'>Packagers</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>security.html</xsl:with-param>
        <xsl:with-param name='label'>Security</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>support.html</xsl:with-param>
        <xsl:with-param name='label'>Support</xsl:with-param>
      </xsl:call-template>
      <xsl:call-template name='page'>
        <xsl:with-param name='href'>technical.html</xsl:with-param>
        <xsl:with-param name='label'>Technical</xsl:with-param>
      </xsl:call-template>
    </div>
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

</xsl:stylesheet>
