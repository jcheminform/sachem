package cz.iocb.orchem.load;

import java.io.File;



public class ChemblCompoundLoader extends CompoundLoader
{
    public static void main(String[] args) throws Exception
    {
        if(args.length != 1)
        {
            System.err.println("missing directory name argument");
            System.exit(1);
        }

        loadDirectory(new File(args[0]), "> <chembl_id>", "CHEMBL");
    }
}